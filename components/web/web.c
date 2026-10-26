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
#include "net.h"
#include "web.h"

#define PAGE_BUF_SIZE 3072

static const char *TAG = "web";

typedef struct {
    const char *version;
    int64_t uptime_s;
    uint32_t free_heap;
    const char *sta_state;
    char sta_ssid[33];
    char sta_ip[16];
    bool sta_has_rssi;
    int sta_rssi;
    const char *ap_ssid;
    char ap_ip[16];
    uint8_t channel;
    int ap_clients;
} status_t;

static void collect(status_t *s)
{
    *s = (status_t){
        .version = esp_app_get_description()->version,
        .uptime_s = esp_timer_get_time() / 1000000,
        .free_heap = esp_get_free_heap_size(),
        .ap_ssid = net_ap_ssid(),
    };

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
    "<tr><td>Uptime<td>%" PRId64 "d %02" PRId64 ":%02" PRId64 ":%02" PRId64
    "<tr><td>Free heap<td>%" PRIu32 " bytes</table>"
    "<h2>Wi-Fi client</h2><table>"
    "<tr><td>State<td>%s<tr><td>Network<td>%s<tr><td>IP<td>%s<tr><td>Signal<td>%s</table>"
    "<h2>Access point</h2><table>"
    "<tr><td>SSID<td>%s<tr><td>IP<td>%s<tr><td>Channel<td>%d<tr><td>Clients<td>%d</table>"
    "<p><a href=/api/status>JSON</a></body></html>";

static const char JSON_FMT[] =
    "{\"version\":\"%s\",\"uptime_s\":%" PRId64 ",\"free_heap\":%" PRIu32 ","
    "\"sta\":{\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%s},"
    "\"ap\":{\"ssid\":\"%s\",\"ip\":\"%s\",\"channel\":%d,\"clients\":%d}}\n";

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
    int64_t t = s->uptime_s;
    return snprintf(buf, len, PAGE_FMT, ap_ssid, ap_ssid,
                    s->version, t / 86400, t / 3600 % 24, t / 60 % 60, t % 60, s->free_heap,
                    s->sta_state, sta_ssid[0] ? sta_ssid : "-", s->sta_ip[0] ? s->sta_ip : "-", rssi,
                    ap_ssid, s->ap_ip, s->channel, s->ap_clients);
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
    return snprintf(buf, len, JSON_FMT, s->version, s->uptime_s, s->free_heap,
                    s->sta_state, sta_ssid, s->sta_ip, rssi,
                    ap_ssid, s->ap_ip, s->channel, s->ap_clients);
}

static esp_err_t handle_index(httpd_req_t *req)
{
    return send_formatted(req, "text/html; charset=utf-8", render_html);
}

static esp_err_t handle_status(httpd_req_t *req)
{
    return send_formatted(req, "application/json", render_json);
}

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "httpd start");

    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = handle_index },
        { .uri = "/api/status", .method = HTTP_GET, .handler = handle_status },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uris[i]), TAG, "register %s", uris[i].uri);
    }
    ESP_LOGI(TAG, "listening on port %d", cfg.server_port);
    return ESP_OK;
}
