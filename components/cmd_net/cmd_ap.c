#include <stdio.h>
#include <string.h>
#include "dhcpserver/dhcpserver.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "shell.h"
#include "net.h"
#include "cmd_net_priv.h"

static const char *TAG = "cmd_ap";

static int cmd_status(int argc, char **argv)
{
    wifi_config_t cfg;
    uint8_t channel, mac[6];
    wifi_second_chan_t second;
    wifi_sta_list_t clients;
    esp_netif_ip_info_t ip;
    dhcps_lease_t lease = { 0 };

    esp_wifi_get_config(WIFI_IF_AP, &cfg);
    esp_wifi_get_channel(&channel, &second);
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    if (esp_wifi_ap_get_sta_list(&clients) != ESP_OK) {
        clients.num = 0;
    }
    esp_netif_get_ip_info(net_ap(), &ip);
    esp_netif_dhcps_option(net_ap(), ESP_NETIF_OP_GET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease));

    printf("ssid       %s\n", net_ap_ssid());
    printf("password   %s\n", cfg.ap.authmode == WIFI_AUTH_OPEN ? "(none)" : (const char *)cfg.ap.password);
    printf("auth       %s\n", net_auth_str(cfg.ap.authmode));
    printf("channel    %d (%s GHz)\n", channel, net_band_str(channel));
    printf("clients    %d/%d\n", clients.num, cfg.ap.max_connection);
    printf("mac        " MACSTR "\n", MAC2STR(mac));
    printf("ip         " IPSTR "/%d\n", IP2STR(&ip.ip), __builtin_popcount(ip.netmask.addr));
    if (lease.enable) {
        printf("dhcp       " IPSTR " - " IPSTR "\n", IP2STR(&lease.start_ip), IP2STR(&lease.end_ip));
    }
    printf("web        http://" IPSTR "/\n", IP2STR(&ip.ip));
    return 0;
}

static int cmd_clients(int argc, char **argv)
{
    wifi_sta_list_t clients;
    if (esp_wifi_ap_get_sta_list(&clients) != ESP_OK) {
        clients.num = 0;
    }
    esp_netif_pair_mac_ip_t pairs[ESP_WIFI_MAX_CONN_NUM] = { 0 };
    for (int i = 0; i < clients.num; i++) {
        memcpy(pairs[i].mac, clients.sta[i].mac, sizeof(pairs[i].mac));
    }
    if (clients.num) {
        esp_netif_dhcps_get_clients_by_mac(net_ap(), clients.num, pairs);
    }

    printf("%-17s %-15s %4s\n", "mac", "ip", "rssi");
    for (int i = 0; i < clients.num; i++) {
        char ip[16] = "-";
        if (pairs[i].ip.addr) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&pairs[i].ip));
        }
        printf(MACSTR " %-15s %4d\n", MAC2STR(clients.sta[i].mac), ip, clients.sta[i].rssi);
    }
    return 0;
}

static const shell_subcmd_t s_subcmds[] = {
    { .name = "status", .help = "Access point config, IP and DHCP range", .func = cmd_status },
    { .name = "clients", .help = "Connected clients: MAC, IP, signal", .func = cmd_clients },
};

static int cmd_ap(int argc, char **argv)
{
    return shell_subcmd_run("ap", s_subcmds, sizeof(s_subcmds) / sizeof(s_subcmds[0]), argc, argv);
}

esp_err_t cmd_ap_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "ap",
        .help = "Access point: status | clients",
        .hint = "<command>",
        .func = cmd_ap,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register ap");
    return ESP_OK;
}
