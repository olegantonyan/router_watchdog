#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "argtable3/argtable3.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "soc/soc_caps.h"
#include "shell.h"
#include "net.h"
#include "cmd_net_priv.h"

static const char *TAG = "cmd_wifi";

static int cmd_scan(int argc, char **argv)
{
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        fprintf(stderr, "scan: %s\n", esp_err_to_name(err));
        return 1;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t *aps = calloc(n ? n : 1, sizeof(*aps));
    if (!aps) {
        esp_wifi_clear_ap_list();
        fprintf(stderr, "scan: out of memory\n");
        return 1;
    }
    esp_wifi_scan_get_ap_records(&n, aps);

    printf("%-32s %-17s %3s %4s %4s %s\n", "ssid", "bssid", "ch", "band", "rssi", "auth");
    for (uint16_t i = 0; i < n; i++) {
        const wifi_ap_record_t *ap = &aps[i];
        printf("%-32.32s " MACSTR " %3d %4s %4d %s\n",
               ap->ssid[0] ? (const char *)ap->ssid : "<hidden>", MAC2STR(ap->bssid),
               ap->primary, net_band_str(ap->primary), ap->rssi, net_auth_str(ap->authmode));
    }
    free(aps);
    return 0;
}

static struct {
    struct arg_int *timeout;
#if SOC_WIFI_SUPPORT_5G
    struct arg_str *band;
#endif
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_end *end;
} s_connect_args;

#if SOC_WIFI_SUPPORT_5G
static bool parse_band(const char *s, wifi_band_mode_t *out)
{
    if (strcmp(s, "2.4") == 0) {
        *out = WIFI_BAND_MODE_2G_ONLY;
    } else if (strcmp(s, "5") == 0) {
        *out = WIFI_BAND_MODE_5G_ONLY;
    } else if (strcmp(s, "auto") == 0) {
        *out = WIFI_BAND_MODE_AUTO;
    } else {
        return false;
    }
    return true;
}
#endif

static void print_ip(void)
{
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(net_sta(), &ip);
    printf("connected, ip " IPSTR "/%d gw " IPSTR "\n", IP2STR(&ip.ip),
           __builtin_popcount(ip.netmask.addr), IP2STR(&ip.gw));
}

static int cmd_connect(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_connect_args)) {
        return 1;
    }
    int timeout_s = s_connect_args.timeout->count ? s_connect_args.timeout->ival[0] : 30;
    const char *ssid = s_connect_args.ssid->sval[0];
    const char *password = s_connect_args.password->count ? s_connect_args.password->sval[0] : NULL;

    net_disconnect();
    esp_err_t err;
#if SOC_WIFI_SUPPORT_5G
    wifi_band_mode_t band = WIFI_BAND_MODE_AUTO;
    if (s_connect_args.band->count && !parse_band(s_connect_args.band->sval[0], &band)) {
        fprintf(stderr, "connect: band must be 2.4, 5 or auto\n");
        return 1;
    }
    err = esp_wifi_set_band_mode(band);
    if (err != ESP_OK) {
        fprintf(stderr, "connect: set band: %s\n", esp_err_to_name(err));
        return 1;
    }
#endif
    err = net_connect(ssid, password);
    if (err != ESP_OK) {
        fprintf(stderr, "connect: %s\n", esp_err_to_name(err));
        net_disconnect();
        return 1;
    }

    printf("connecting to '%s' (Ctrl+C to abort)\n", ssid);
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_s * 1000000;
    while (!net_has_ip()) {
        if (shell_getc(100) == SHELL_CTRL_C) {
            net_disconnect();
            fprintf(stderr, "connect: aborted\n");
            return 1;
        }
        if (esp_timer_get_time() > deadline) {
            if (net_associated()) {
                fprintf(stderr, "connect: associated but no IP after %d s, still trying; see 'wifi status'\n", timeout_s);
                return 1;
            }
            uint8_t reason = net_last_reason();
            net_disconnect();
            fprintf(stderr, "connect: timeout after %d s", timeout_s);
            if (reason) {
                fprintf(stderr, ", last error: %s (%d)", net_reason_str(reason), reason);
            }
            fprintf(stderr, "\n");
            return 1;
        }
    }
    print_ip();
    return 0;
}

static int cmd_disconnect(int argc, char **argv)
{
    net_disconnect();
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    wifi_ap_record_t ap;
    bool associated = net_associated() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    const char *state = net_has_ip() ? "connected"
                        : associated ? "associated, waiting for ip"
                        : net_reconnect_enabled() ? "connecting"
                        : "idle";
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    printf("state      %s\n", state);
    printf("mac        " MACSTR "\n", MAC2STR(mac));
    if (associated) {
        printf("ssid       %s\n", (const char *)ap.ssid);
        printf("bssid      " MACSTR "\n", MAC2STR(ap.bssid));
        printf("channel    %d (%s GHz)\n", ap.primary, net_band_str(ap.primary));
        printf("rssi       %d dBm\n", ap.rssi);
        printf("auth       %s\n", net_auth_str(ap.authmode));
    }
    if (net_has_ip()) {
        esp_netif_ip_info_t ip;
        esp_netif_dns_info_t dns;
        esp_netif_get_ip_info(net_sta(), &ip);
        printf("ip         " IPSTR "\n", IP2STR(&ip.ip));
        printf("netmask    " IPSTR "\n", IP2STR(&ip.netmask));
        printf("gateway    " IPSTR "\n", IP2STR(&ip.gw));
        if (esp_netif_get_dns_info(net_sta(), ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            printf("dns        " IPSTR "\n", IP2STR(&dns.ip.u_addr.ip4));
        }
    }
    uint8_t reason = net_last_reason();
    if (!associated && reason) {
        printf("last error %s (%d)\n", net_reason_str(reason), reason);
    }
    return 0;
}

static const shell_subcmd_t s_subcmds[] = {
    { .name = "scan", .help = "List nearby networks", .func = cmd_scan },
    { .name = "connect", .help = "Join a network (RAM only, not saved)", .func = cmd_connect, .argtable = &s_connect_args },
    { .name = "disconnect", .help = "Leave the network and stop reconnecting", .func = cmd_disconnect },
    { .name = "status", .help = "Connection state, signal and IP config", .func = cmd_status },
};

static int cmd_wifi(int argc, char **argv)
{
    return shell_subcmd_run("wifi", s_subcmds, sizeof(s_subcmds) / sizeof(s_subcmds[0]), argc, argv);
}

esp_err_t cmd_wifi_register(void)
{
    s_connect_args.timeout = arg_int0("t", "timeout", "<sec>", "wait for IP, default 30");
#if SOC_WIFI_SUPPORT_5G
    s_connect_args.band = arg_str0("b", "band", "<2.4|5|auto>", "radio band, default auto");
#endif
    s_connect_args.ssid = arg_str1(NULL, NULL, "<ssid>", "network name");
    s_connect_args.password = arg_str0(NULL, NULL, "<password>", "omit for open networks");
    s_connect_args.end = arg_end(4);

    const esp_console_cmd_t cmd = {
        .command = "wifi",
        .help = "Wi-Fi client: scan | connect | disconnect | status",
        .hint = "<command> [args]",
        .func = cmd_wifi,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register wifi");
    return ESP_OK;
}
