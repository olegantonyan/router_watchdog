#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"
#include "sdkconfig.h"
#include "net.h"
#include "inet.h"

#define RETRY_INTERVAL_S 10
#define HTTP_TIMEOUT_MS  5000
#define PROBE_STACK      3072

typedef struct {
    const char *name;
    const char *target;
    const char *url;
    int status;
    const char *body;
} probe_def_t;

static const probe_def_t s_defs[] = {
    { .name = "gateway" },
    { .name = "ping", .target = "1.1.1.1" },
    { .name = "ping", .target = "8.8.8.8" },
    { .name = "google", .target = "connectivitycheck.gstatic.com", .url = "http://connectivitycheck.gstatic.com/generate_204", .status = 204 },
    { .name = "cloudflare", .target = "cp.cloudflare.com", .url = "http://cp.cloudflare.com/generate_204", .status = 204 },
    { .name = "microsoft", .target = "www.msftconnecttest.com", .url = "http://www.msftconnecttest.com/connecttest.txt", .status = 200, .body = "Microsoft Connect Test" },
    { .name = "apple", .target = "captive.apple.com", .url = "http://captive.apple.com/hotspot-detect.html", .status = 200, .body = "Success" },
};
#define PROBE_COUNT (sizeof(s_defs) / sizeof(s_defs[0]))
_Static_assert(PROBE_COUNT <= INET_MAX_PROBES, "raise INET_MAX_PROBES");

static const char *TAG = "inet";

static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_done;
static volatile inet_state_t s_state;
static inet_report_t s_report;
static inet_probe_t s_work[PROBE_COUNT];
static uint32_t s_fail_streak;

typedef struct {
    TaskHandle_t waiter;
    uint32_t replies;
    uint32_t sum_ms;
} ping_ctx_t;

typedef struct {
    const probe_def_t *def;
    inet_probe_t *probe;
    EventBits_t bit;
} job_t;

static void on_ping_success(esp_ping_handle_t hdl, void *arg)
{
    ping_ctx_t *ctx = arg;
    uint32_t elapsed;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    ctx->replies++;
    ctx->sum_ms += elapsed;
}

static void on_ping_end(esp_ping_handle_t hdl, void *arg)
{
    xTaskNotifyGive(((ping_ctx_t *)arg)->waiter);
}

static bool probe_ping(inet_probe_t *p)
{
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = 3;
    cfg.interval_ms = 200;
    cfg.timeout_ms = 1000;
    if (!ipaddr_aton(p->target, &cfg.target_addr)) {
        strlcpy(p->error, "bad address", sizeof(p->error));
        return false;
    }
    ping_ctx_t ctx = { .waiter = xTaskGetCurrentTaskHandle() };
    esp_ping_callbacks_t cbs = { .cb_args = &ctx, .on_ping_success = on_ping_success, .on_ping_end = on_ping_end };
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
        strlcpy(p->error, "out of memory", sizeof(p->error));
        return false;
    }
    ulTaskNotifyTake(pdTRUE, 0);
    esp_ping_start(ping);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    esp_ping_delete_session(ping);
    if (!ctx.replies) {
        strlcpy(p->error, "no reply", sizeof(p->error));
        return false;
    }
    p->ms = ctx.sum_ms / ctx.replies;
    return true;
}

static bool probe_http(const probe_def_t *d, inet_probe_t *p)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(d->target, NULL, &hints, &res) != 0 || !res) {
        strlcpy(p->error, "dns failed", sizeof(p->error));
        return false;
    }
    freeaddrinfo(res);

    esp_http_client_config_t cfg = {
        .url = d->url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .buffer_size = 512,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        strlcpy(p->error, "out of memory", sizeof(p->error));
        return false;
    }
    bool ok = false;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        strlcpy(p->error, "connect failed", sizeof(p->error));
    } else if (esp_http_client_fetch_headers(c) < 0) {
        strlcpy(p->error, "no response", sizeof(p->error));
    } else if (esp_http_client_get_status_code(c) != d->status) {
        snprintf(p->error, sizeof(p->error), "status %d", esp_http_client_get_status_code(c));
    } else if (d->body) {
        char body[64];
        int n = esp_http_client_read_response(c, body, sizeof(body) - 1);
        body[n > 0 ? n : 0] = '\0';
        ok = strstr(body, d->body) != NULL;
        if (!ok) {
            strlcpy(p->error, "unexpected body", sizeof(p->error));
        }
    } else {
        ok = true;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

static bool public_ping_ok(void)
{
    for (size_t i = 1; i < PROBE_COUNT; i++) {
        if (!s_work[i].http && s_work[i].ok) {
            return true;
        }
    }
    return false;
}

static const char *describe_failure(void)
{
    if (!s_work[0].ok) {
        return "gateway unreachable";
    }
    if (!public_ping_ok()) {
        return "public IPs unreachable";
    }
    for (size_t i = 1; i < PROBE_COUNT; i++) {
        if (s_work[i].http && strcmp(s_work[i].error, "dns failed") != 0) {
            return "http checks failing";
        }
    }
    return "dns failing";
}

static void run_probe(job_t *job)
{
    inet_probe_t *p = job->probe;
    int64_t start = esp_timer_get_time();
    p->ok = p->http ? probe_http(job->def, p) : probe_ping(p);
    if (p->http) {
        p->ms = (esp_timer_get_time() - start) / 1000;
    }
}

static void probe_task(void *arg)
{
    job_t *job = arg;
    run_probe(job);
    xEventGroupSetBits(s_done, job->bit);
    vTaskDelete(NULL);
}

static void set_state(inet_state_t state, const char *reason)
{
    if (state != s_state) {
        s_report.state_since_us = esp_timer_get_time();
        if (state == INET_ONLINE) {
            ESP_LOGI(TAG, "internet online");
        } else if (state == INET_OFFLINE) {
            ESP_LOGW(TAG, "internet offline: %s", reason);
        }
    }
    s_state = state;
    s_report.state = state;
    strlcpy(s_report.reason, reason, sizeof(s_report.reason));
}

static bool run_round(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_report.running = true;
    xSemaphoreGive(s_lock);

    static job_t jobs[PROBE_COUNT];
    EventBits_t all = 0;
    for (size_t i = 0; i < PROBE_COUNT; i++) {
        const probe_def_t *d = &s_defs[i];
        inet_probe_t *p = &s_work[i];
        *p = (inet_probe_t){ .name = d->name, .http = d->url != NULL };
        if (d->target) {
            strlcpy(p->target, d->target, sizeof(p->target));
        } else {
            esp_netif_ip_info_t ip;
            esp_netif_get_ip_info(net_sta(), &ip);
            snprintf(p->target, sizeof(p->target), IPSTR, IP2STR(&ip.gw));
        }
        jobs[i] = (job_t){ .def = d, .probe = p, .bit = BIT(i) };
        all |= BIT(i);
    }
    xEventGroupClearBits(s_done, all);
    for (size_t i = 0; i < PROBE_COUNT; i++) {
        if (xTaskCreate(probe_task, "inet_probe", PROBE_STACK, &jobs[i], 3, NULL) != pdPASS) {
            run_probe(&jobs[i]);
            xEventGroupSetBits(s_done, jobs[i].bit);
        }
    }
    xEventGroupWaitBits(s_done, all, pdTRUE, pdTRUE, portMAX_DELAY);

    bool http_ok = false;
    for (size_t i = 0; i < PROBE_COUNT; i++) {
        http_ok |= s_work[i].http && s_work[i].ok;
    }
    bool complete = net_has_ip();
    const char *reason = "";
    bool total_outage = false;
    if (complete) {
        if (http_ok) {
            s_fail_streak = 0;
        } else {
            s_fail_streak++;
            reason = describe_failure();
            total_outage = !public_ping_ok();
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_report.running = false;
    if (complete) {
        s_report.fail_streak = s_fail_streak;
        bool offline = !http_ok && (s_state != INET_ONLINE || total_outage || s_fail_streak >= CONFIG_RW_INET_FAIL_ROUNDS);
        set_state(offline ? INET_OFFLINE : INET_ONLINE, reason);
        memcpy(s_report.probes, s_work, sizeof(s_work));
        s_report.count = PROBE_COUNT;
        s_report.last_round_us = esp_timer_get_time();
        s_report.rounds++;
    }
    xSemaphoreGive(s_lock);
    return complete;
}

static void set_unknown(void)
{
    s_fail_streak = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_report.fail_streak = 0;
    set_state(INET_UNKNOWN, "");
    s_report.next_round_us = 0;
    xSemaphoreGive(s_lock);
}

static void inet_task(void *arg)
{
    for (;;) {
        if (!net_has_ip()) {
            if (s_state != INET_UNKNOWN) {
                set_unknown();
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            continue;
        }
        if (!run_round()) {
            continue;
        }
        uint32_t wait_s = s_fail_streak ? RETRY_INTERVAL_S : CONFIG_RW_INET_INTERVAL;
        int64_t next = esp_timer_get_time() + (int64_t)wait_s * 1000000;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_report.next_round_us = next;
        xSemaphoreGive(s_lock);
        while (net_has_ip() && esp_timer_get_time() < next) {
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500))) {
                break;
            }
        }
    }
}

esp_err_t inet_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_done = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_lock && s_done, ESP_ERR_NO_MEM, TAG, "sync objects");
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);
    esp_log_level_set("esp-tls", ESP_LOG_NONE);
    esp_log_level_set("transport_base", ESP_LOG_NONE);
    ESP_RETURN_ON_FALSE(xTaskCreate(inet_task, "inet", 4096, NULL, 3, &s_task) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

inet_state_t inet_state(void)
{
    return s_state;
}

const char *inet_state_str(inet_state_t state)
{
    switch (state) {
    case INET_ONLINE: return "online";
    case INET_OFFLINE: return "offline";
    default: return "unknown";
    }
}

void inet_report(inet_report_t *out)
{
    if (!s_lock) {
        *out = (inet_report_t){ 0 };
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_report;
    xSemaphoreGive(s_lock);
}

void inet_check_now(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}
