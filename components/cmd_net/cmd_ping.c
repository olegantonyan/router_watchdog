#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "argtable3/argtable3.h"
#include "esp_check.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"
#include "shell.h"
#include "net.h"
#include "cmd_net_priv.h"

static const char *TAG = "cmd_ping";

static struct {
    struct arg_int *count;
    struct arg_int *interval;
    struct arg_int *size;
    struct arg_int *timeout;
    struct arg_str *host;
    struct arg_end *end;
} s_ping_args;

typedef struct {
    TaskHandle_t waiter;
    uint32_t min_ms;
    uint32_t max_ms;
    uint32_t sum_ms;
} ping_stats_t;

static void on_success(esp_ping_handle_t hdl, void *arg)
{
    ping_stats_t *st = arg;
    uint16_t seq;
    uint8_t ttl;
    uint32_t elapsed, size, received;
    ip_addr_t addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &addr, sizeof(addr));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &size, sizeof(size));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));

    if (received == 1 || elapsed < st->min_ms) {
        st->min_ms = elapsed;
    }
    if (elapsed > st->max_ms) {
        st->max_ms = elapsed;
    }
    st->sum_ms += elapsed;
    printf("%" PRIu32 " bytes from %s: icmp_seq=%u ttl=%u time=%" PRIu32 " ms\n",
           size, ipaddr_ntoa(&addr), seq, ttl, elapsed);
}

static void on_timeout(esp_ping_handle_t hdl, void *arg)
{
    uint16_t seq;
    ip_addr_t addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &addr, sizeof(addr));
    printf("from %s: icmp_seq=%u timeout\n", ipaddr_ntoa(&addr), seq);
}

static void on_end(esp_ping_handle_t hdl, void *arg)
{
    xTaskNotifyGive(((ping_stats_t *)arg)->waiter);
}

static bool resolve(const char *host, ip_addr_t *out)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        return false;
    }
    if (res->ai_family == AF_INET) {
        inet_addr_to_ip4addr(ip_2_ip4(out), &((struct sockaddr_in *)res->ai_addr)->sin_addr);
        IP_SET_TYPE_VAL(*out, IPADDR_TYPE_V4);
    } else {
        inet6_addr_to_ip6addr(ip_2_ip6(out), &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr);
        IP_SET_TYPE_VAL(*out, IPADDR_TYPE_V6);
    }
    freeaddrinfo(res);
    return true;
}

static int cmd_ping(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_ping_args)) {
        return 1;
    }
    int count = s_ping_args.count->count ? s_ping_args.count->ival[0] : 4;
    int interval = s_ping_args.interval->count ? s_ping_args.interval->ival[0] : 1000;
    int size = s_ping_args.size->count ? s_ping_args.size->ival[0] : 56;
    int timeout = s_ping_args.timeout->count ? s_ping_args.timeout->ival[0] : 1000;
    if (count < 0 || interval <= 0 || size < 0 || size > 1400 || timeout <= 0) {
        fprintf(stderr, "ping: invalid argument (count >= 0, interval > 0, size 0..1400, timeout > 0)\n");
        return 1;
    }

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = count;
    cfg.interval_ms = interval;
    cfg.data_size = size;
    cfg.timeout_ms = timeout;

    const char *host = s_ping_args.host->sval[0];
    if (!net_has_ip()) {
        fprintf(stderr, "ping: network is down, see 'wifi connect'\n");
        return 1;
    }
    if (!resolve(host, &cfg.target_addr)) {
        fprintf(stderr, "ping: cannot resolve %s\n", host);
        return 1;
    }

    ping_stats_t stats = { .waiter = xTaskGetCurrentTaskHandle() };
    esp_ping_callbacks_t cbs = {
        .cb_args = &stats,
        .on_ping_success = on_success,
        .on_ping_timeout = on_timeout,
        .on_ping_end = on_end,
    };
    esp_ping_handle_t ping;
    esp_err_t err = esp_ping_new_session(&cfg, &cbs, &ping);
    if (err != ESP_OK) {
        fprintf(stderr, "ping: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("PING %s (%s): %" PRIu32 " data bytes\n", host, ipaddr_ntoa(&cfg.target_addr), cfg.data_size);
    ulTaskNotifyTake(pdTRUE, 0);
    esp_ping_start(ping);
    while (!ulTaskNotifyTake(pdTRUE, 0)) {
        if (shell_getc(100) == SHELL_CTRL_C) {
            esp_ping_stop(ping);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(cfg.interval_ms + cfg.timeout_ms + 1000));
            break;
        }
    }

    uint32_t sent, received;
    esp_ping_get_profile(ping, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(ping, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_delete_session(ping);

    printf("--- %s ping statistics ---\n", host);
    printf("%" PRIu32 " transmitted, %" PRIu32 " received, %" PRIu32 "%% loss\n",
           sent, received, sent ? (sent - received) * 100 / sent : 0);
    if (received) {
        printf("rtt min/avg/max = %" PRIu32 "/%" PRIu32 "/%" PRIu32 " ms\n",
               stats.min_ms, stats.sum_ms / received, stats.max_ms);
    }
    return received ? 0 : 1;
}

esp_err_t cmd_ping_register(void)
{
    s_ping_args.count = arg_int0("c", "count", "<n>", "number of pings, 0 = until Ctrl+C, default 4");
    s_ping_args.interval = arg_int0("i", "interval", "<ms>", "interval, default 1000");
    s_ping_args.size = arg_int0("s", "size", "<bytes>", "payload size, default 56");
    s_ping_args.timeout = arg_int0("W", "timeout", "<ms>", "reply timeout, default 1000");
    s_ping_args.host = arg_str1(NULL, NULL, "<host>", "hostname or IP");
    s_ping_args.end = arg_end(4);

    const esp_console_cmd_t cmd = {
        .command = "ping",
        .help = "Send ICMP echo requests",
        .func = cmd_ping,
        .argtable = &s_ping_args,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register ping");
    return ESP_OK;
}
