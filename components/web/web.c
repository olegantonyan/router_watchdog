#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "clock.h"
#include "inet.h"
#include "net.h"
#include "ota.h"
#include "relay.h"
#include "shell.h"
#include "watchdog.h"
#include "web.h"
#include "web_shell.h"

#define PAGE_BUF_SIZE 4096
#define UPLOAD_CHUNK 4096
#define UPLOAD_TIMEOUTS 3

static const char *TAG = "web";

typedef struct {
    const char *version;
    char time[32];
    bool time_synced;
    int64_t uptime_s;
    uint32_t free_heap;
    const char *sta_state;
    const char *internet;
    char sta_ssid[33];
    char sta_ip[16];
    bool sta_has_rssi;
    int sta_rssi;
    const char *ap_ssid;
    char ap_ip[16];
    uint8_t channel;
    int ap_clients;
    bool wd_running;
    bool wd_enabled;
    char wd_state[64];
    uint32_t wd_cycles;
    int64_t wd_last_ago_s;
    char wd_last_reason[32];
    const char *ota_partition;
    const char *ota_state;
    char ota_other[64];
    int relay_gpio[RELAY_COUNT];
    bool relay_on[RELAY_COUNT];
} status_t;

static void collect(status_t *s)
{
    *s = (status_t){
        .version = esp_app_get_description()->version,
        .uptime_s = esp_timer_get_time() / 1000000,
        .free_heap = esp_get_free_heap_size(),
        .ap_ssid = net_ap_ssid(),
        .time_synced = clock_sync_count() > 0,
        .internet = inet_state_str(inet_state()),
    };
    if (clock_is_set()) {
        time_t now = time(NULL);
        struct tm tm;
        strftime(s->time, sizeof(s->time), "%Y-%m-%dT%H:%M:%S%z", localtime_r(&now, &tm));
    }

    wifi_ap_record_t ap;
    bool associated = net_associated() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    s->sta_state = net_has_ip() ? "connected"
                   : associated ? "associated"
                   : net_reconnect_enabled() ? "connecting"
                   : "idle";
    if (associated) {
        strlcpy(s->sta_ssid, (const char *)ap.ssid, sizeof(s->sta_ssid));
        s->sta_has_rssi = true;
        s->sta_rssi = ap.rssi;
    }
    esp_netif_ip_info_t ip;
    if (net_has_ip() && esp_netif_get_ip_info(net_sta(), &ip) == ESP_OK) {
        snprintf(s->sta_ip, sizeof(s->sta_ip), IPSTR, IP2STR(&ip.ip));
    }
    if (esp_netif_get_ip_info(net_ap(), &ip) == ESP_OK) {
        snprintf(s->ap_ip, sizeof(s->ap_ip), IPSTR, IP2STR(&ip.ip));
    }

    wifi_second_chan_t second;
    esp_wifi_get_channel(&s->channel, &second);
    wifi_sta_list_t clients;
    s->ap_clients = esp_wifi_ap_get_sta_list(&clients) == ESP_OK ? clients.num : 0;

    for (int i = 0; i < RELAY_COUNT; i++) {
        s->relay_gpio[i] = relay_gpio(i + 1);
        s->relay_on[i] = s->relay_gpio[i] >= 0 && relay_get(i + 1);
    }

    ota_status_t ota;
    ota_status(&ota);
    s->ota_partition = ota.running;
    s->ota_state = ota_state_str(&ota);
    snprintf(s->ota_other, sizeof(s->ota_other), "%s: %s", ota.other ? ota.other : "-", ota.other_version[0] ? ota.other_version : "empty");

    watchdog_status_t wd;
    watchdog_status(&wd);
    int64_t now = esp_timer_get_time();
    char d[16], d2[16];
    s->wd_running = wd.relay != 0;
    s->wd_enabled = wd.enabled;
    s->wd_cycles = wd.cycles;
    s->wd_last_ago_s = wd.cycles ? (now - wd.last_cycle_us) / 1000000 : -1;
    strlcpy(s->wd_last_reason, wd.last_reason, sizeof(s->wd_last_reason));
    if (!s->wd_running) {
        strcpy(s->wd_state, "not running");
    } else if (wd.cycling) {
        strcpy(s->wd_state, "power cycling");
    } else if (!wd.enabled) {
        strcpy(s->wd_state, "disabled");
    } else if (wd.holdoff_until_us > now) {
        snprintf(s->wd_state, sizeof(s->wd_state), "waiting %s after %s", shell_fmt_duration(d, sizeof(d), wd.holdoff_until_us - now), wd.cycles ? "last cycle" : "boot");
    } else if (wd.pending) {
        snprintf(s->wd_state, sizeof(s->wd_state), "%s for %s of %s", wd.pending, shell_fmt_duration(d, sizeof(d), wd.pending_for_us), shell_fmt_duration(d2, sizeof(d2), wd.pending_limit_s * 1000000LL));
    } else {
        strcpy(s->wd_state, "armed");
    }
}

static const char *escape(char *dst, size_t len, const char *src, bool json)
{
    size_t o = 0;
    for (; *src && o + 7 < len; src++) {
        unsigned char c = *src;
        const char *rep = NULL;
        if (json) {
            rep = c == '"' ? "\\\"" : c == '\\' ? "\\\\" : NULL;
        } else {
            rep = c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '"' ? "&quot;" : NULL;
        }
        if (rep) {
            o += snprintf(dst + o, len - o, "%s", rep);
        } else if (c >= 0x20) {
            dst[o++] = c;
        }
    }
    dst[o] = '\0';
    return dst;
}

static const char PAGE_FMT[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<meta name=color-scheme content=\"light dark\"><meta http-equiv=refresh content=5>"
    "<title>%s</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:32rem;margin:1rem auto;padding:0 1rem}"
    "h1{font-size:1.3rem}h2{font-size:1rem;margin-top:1.5rem}"
    "table{border-collapse:collapse;width:100%%}td{padding:.3rem 0;border-bottom:1px solid #8884}"
    "td:first-child{opacity:.6;width:40%%}"
    "</style></head><body><h1>%s</h1>"
    "<h2>Device</h2><table>"
    "<tr><td>Version<td>%s"
    "<tr><td>Time<td>%s"
    "<tr><td>Uptime<td>%" PRId64 "d %02" PRId64 ":%02" PRId64 ":%02" PRId64
    "<tr><td>Free heap<td>%" PRIu32 " bytes</table>"
    "<h2>Wi-Fi client</h2><table>"
    "<tr><td>State<td>%s<tr><td>Internet<td>%s<tr><td>Network<td>%s<tr><td>IP<td>%s<tr><td>Signal<td>%s</table>"
    "<h2>Access point</h2><table>"
    "<tr><td>SSID<td>%s<tr><td>IP<td>%s<tr><td>Channel<td>%d<tr><td>Clients<td>%d</table>"
    "<h2>Relays</h2><table>%s</table>"
    "<h2>Watchdog</h2><table>"
    "<tr><td>State<td>%s<tr><td>Power cycles<td>%" PRIu32 "<tr><td>Last cycle<td>%s</table>"
    "<p><a href=/shell>Shell</a> · <a href=/update>Firmware update</a> · <a href=/api/status>JSON</a></body></html>";

static const char UPDATE_FMT[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<meta name=color-scheme content=\"light dark\">"
    "<title>%s update</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:32rem;margin:1rem auto;padding:0 1rem}"
    "h1{font-size:1.3rem}"
    "table{border-collapse:collapse;width:100%%}td{padding:.3rem 0;border-bottom:1px solid #8884}"
    "td:first-child{opacity:.6;width:40%%}progress{width:100%%}"
    "</style></head><body><h1>Firmware update</h1><table>"
    "<tr><td>Version<td>%s<tr><td>Partition<td>%s<tr><td>State<td>%s<tr><td>Other slot<td>%s</table>"
    "<p><input type=file id=f accept=.bin> <button id=b>Upload</button>"
    "<p><progress id=p max=1 value=0 hidden></progress><p id=m>"
    "<p><a href=/>Status</a>"
    "<script>"
    "const $=i=>document.getElementById(i),run='%s';"
    "function wait(){setTimeout(()=>fetch('/api/status').then(r=>r.json()).then(s=>{"
    "if(s.ota.partition==run)return wait();$('m').textContent='Running '+s.version+' on '+s.ota.partition}).catch(wait),3000)}"
    "$('b').onclick=()=>{const f=$('f').files[0];if(!f){$('m').textContent='Choose a .bin file';return}"
    "$('b').disabled=true;$('p').hidden=false;const x=new XMLHttpRequest();"
    "x.upload.onprogress=e=>{$('p').value=e.loaded/e.total;$('m').textContent='Uploading '+Math.round(e.loaded*100/e.total)+'%%'};"
    "x.onload=()=>{$('m').textContent=x.responseText;if(x.status==200){$('m').textContent+=' Waiting for the board...';wait()}else $('b').disabled=false};"
    "x.onerror=()=>{$('m').textContent='Upload failed';$('b').disabled=false};"
    "x.open('POST','/api/update');x.send(f)}"
    "</script></body></html>";

static const char JSON_FMT[] =
    "{\"version\":\"%s\",\"time\":%s,\"time_synced\":%s,\"uptime_s\":%" PRId64 ",\"free_heap\":%" PRIu32 ","
    "\"internet\":\"%s\",\"sta\":{\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%s},"
    "\"ap\":{\"ssid\":\"%s\",\"ip\":\"%s\",\"channel\":%d,\"clients\":%d},"
    "\"watchdog\":{\"running\":%s,\"enabled\":%s,\"state\":\"%s\",\"cycles\":%" PRIu32 ",\"last_cycle_ago_s\":%s,\"last_reason\":\"%s\"},"
    "\"ota\":{\"partition\":\"%s\",\"state\":\"%s\"},\"relays\":[%s]}\n";

static esp_err_t send_formatted(httpd_req_t *req, const char *type, int (*render)(char *, size_t, const status_t *))
{
    char *buf = malloc(PAGE_BUF_SIZE);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    status_t s;
    collect(&s);
    int n = render(buf, PAGE_BUF_SIZE, &s);
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, buf, n < PAGE_BUF_SIZE ? n : PAGE_BUF_SIZE - 1);
    free(buf);
    return err;
}

static int render_html(char *buf, size_t len, const status_t *s)
{
    char ap_ssid[200], sta_ssid[200], rssi[16];
    escape(ap_ssid, sizeof(ap_ssid), s->ap_ssid, false);
    escape(sta_ssid, sizeof(sta_ssid), s->sta_ssid, false);
    if (s->sta_has_rssi) {
        snprintf(rssi, sizeof(rssi), "%d dBm", s->sta_rssi);
    } else {
        strcpy(rssi, "-");
    }
    char time[48];
    if (s->time[0]) {
        snprintf(time, sizeof(time), "%s%s", s->time, s->time_synced ? "" : " (not synced)");
    } else {
        strcpy(time, "not set");
    }
    char last[64] = "-";
    if (s->wd_last_ago_s >= 0) {
        char d[16];
        snprintf(last, sizeof(last), "%s ago (%s)", shell_fmt_duration(d, sizeof(d), s->wd_last_ago_s * 1000000LL), s->wd_last_reason);
    }
    char relays[RELAY_COUNT * 64];
    size_t o = 0;
    for (int i = 0; i < RELAY_COUNT; i++) {
        int g = s->relay_gpio[i];
        char pin[24] = "";
        if (g >= 0) {
            snprintf(pin, sizeof(pin), " (GPIO%d)", g);
        }
        o += snprintf(relays + o, sizeof(relays) - o, "<tr><td>%d%s%s<td>%s", i + 1, pin, i + 1 == CONFIG_RW_WD_RELAY ? ", router" : "",
                      g < 0 ? "not connected" : s->relay_on[i] ? "on" : "off");
    }
    int64_t t = s->uptime_s;
    return snprintf(buf, len, PAGE_FMT, ap_ssid, ap_ssid,
                    s->version, time, t / 86400, t / 3600 % 24, t / 60 % 60, t % 60, s->free_heap,
                    s->sta_state, s->internet, sta_ssid[0] ? sta_ssid : "-", s->sta_ip[0] ? s->sta_ip : "-", rssi,
                    ap_ssid, s->ap_ip, s->channel, s->ap_clients,
                    relays, s->wd_state, s->wd_cycles, last);
}

static int render_json(char *buf, size_t len, const status_t *s)
{
    char ap_ssid[200], sta_ssid[200], rssi[8];
    escape(ap_ssid, sizeof(ap_ssid), s->ap_ssid, true);
    escape(sta_ssid, sizeof(sta_ssid), s->sta_ssid, true);
    if (s->sta_has_rssi) {
        snprintf(rssi, sizeof(rssi), "%d", s->sta_rssi);
    } else {
        strcpy(rssi, "null");
    }
    char time[40];
    if (s->time[0]) {
        snprintf(time, sizeof(time), "\"%s\"", s->time);
    } else {
        strcpy(time, "null");
    }
    char last_ago[24] = "null";
    if (s->wd_last_ago_s >= 0) {
        snprintf(last_ago, sizeof(last_ago), "%" PRId64, s->wd_last_ago_s);
    }
    char relays[RELAY_COUNT * 40];
    size_t o = 0;
    for (int i = 0; i < RELAY_COUNT; i++) {
        char gpio[12] = "null";
        if (s->relay_gpio[i] >= 0) {
            snprintf(gpio, sizeof(gpio), "%d", s->relay_gpio[i]);
        }
        o += snprintf(relays + o, sizeof(relays) - o, "%s{\"relay\":%d,\"gpio\":%s,\"on\":%s}", i ? "," : "", i + 1, gpio, s->relay_on[i] ? "true" : "false");
    }
    return snprintf(buf, len, JSON_FMT, s->version, time, s->time_synced ? "true" : "false", s->uptime_s, s->free_heap,
                    s->internet, s->sta_state, sta_ssid, s->sta_ip, rssi,
                    ap_ssid, s->ap_ip, s->channel, s->ap_clients,
                    s->wd_running ? "true" : "false", s->wd_enabled ? "true" : "false", s->wd_state, s->wd_cycles, last_ago, s->wd_last_reason,
                    s->ota_partition, s->ota_state, relays);
}

static int render_update(char *buf, size_t len, const status_t *s)
{
    char ap_ssid[200];
    escape(ap_ssid, sizeof(ap_ssid), s->ap_ssid, false);
    return snprintf(buf, len, UPDATE_FMT, ap_ssid, s->version, s->ota_partition, s->ota_state, s->ota_other, s->ota_partition);
}

static esp_err_t handle_index(httpd_req_t *req)
{
    return send_formatted(req, "text/html; charset=utf-8", render_html);
}

static esp_err_t handle_status(httpd_req_t *req)
{
    return send_formatted(req, "application/json", render_json);
}

static esp_err_t handle_update_page(httpd_req_t *req)
{
    return send_formatted(req, "text/html; charset=utf-8", render_update);
}

static esp_err_t upload_error(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, msg);
    return ESP_FAIL;
}

static esp_err_t handle_update(httpd_req_t *req)
{
    if (req->content_len == 0) {
        return upload_error(req, "400 Bad Request", "empty body, POST the .bin file");
    }
    const char *err = ota_begin(req->content_len);
    if (err) {
        return upload_error(req, "409 Conflict", err);
    }
    char *buf = malloc(UPLOAD_CHUNK);
    if (!buf) {
        ota_finish(false);
        return upload_error(req, "500 Internal Server Error", "out of memory");
    }
    size_t left = req->content_len;
    int timeouts = 0;
    while (left) {
        int n = httpd_req_recv(req, buf, left < UPLOAD_CHUNK ? left : UPLOAD_CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < UPLOAD_TIMEOUTS) {
            continue;
        }
        if (n <= 0) {
            free(buf);
            ota_finish(false);
            ESP_LOGW(TAG, "upload interrupted, %zu bytes missing", left);
            return ESP_FAIL;
        }
        timeouts = 0;
        left -= n;
        if (!err) {
            err = ota_write(buf, n);
        }
    }
    free(buf);
    const char *fin = ota_finish(!err);
    if (err || fin) {
        return upload_error(req, "400 Bad Request", err ? err : fin);
    }

    ota_status_t st;
    ota_status(&st);
    char msg[96];
    snprintf(msg, sizeof(msg), "Installed %s on %s, rebooting.", st.other_version, st.other);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);
    ota_reboot(1000);
    return ESP_OK;
}

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "httpd start");

    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = handle_index },
        { .uri = "/api/status", .method = HTTP_GET, .handler = handle_status },
        { .uri = "/update", .method = HTTP_GET, .handler = handle_update_page },
        { .uri = "/api/update", .method = HTTP_POST, .handler = handle_update },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uris[i]), TAG, "register %s", uris[i].uri);
    }
    ESP_RETURN_ON_ERROR(web_shell_register(server), TAG, "shell");
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    ESP_LOGI(TAG, "listening on port %d", cfg.server_port);
    return ESP_OK;
}
