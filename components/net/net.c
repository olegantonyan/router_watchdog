#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"
#include "net.h"

#define NET_ASSOCIATED BIT0
#define NET_GOT_IP     BIT1

_Static_assert(sizeof(CONFIG_RW_AP_PASSWORD) == 1 || (sizeof(CONFIG_RW_AP_PASSWORD) >= 9 && sizeof(CONFIG_RW_AP_PASSWORD) <= 64),
               "CONFIG_RW_AP_PASSWORD must be empty or 8-63 characters");

static const char *TAG = "net";

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static char s_ap_ssid[33];
static volatile bool s_reconnect;
static volatile bool s_leaving;
static volatile uint8_t s_last_reason;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_last_reason = 0;
        xEventGroupSetBits(s_events, NET_ASSOCIATED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = data;
        xEventGroupClearBits(s_events, NET_ASSOCIATED | NET_GOT_IP);
        if (s_leaving) {
            s_leaving = false;
            return;
        }
        ESP_LOGW(TAG, "disconnected: %s (%d)%s", net_reason_str(e->reason), e->reason,
                 s_reconnect ? ", reconnecting" : "");
        if (s_reconnect) {
            s_last_reason = e->reason;
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "ap client " MACSTR " joined", MAC2STR(e->mac));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "ap client " MACSTR " left", MAC2STR(e->mac));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, NET_GOT_IP);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(s_events, NET_GOT_IP);
    }
}

static esp_err_t ap_configure(void)
{
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "ap mac");
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%.27s-%02X%02X", CONFIG_RW_AP_SSID, mac[4], mac[5]);

    wifi_config_t cfg = {
        .ap = {
            .ssid_len = strlen(s_ap_ssid),
            .password = CONFIG_RW_AP_PASSWORD,
            .channel = CONFIG_RW_AP_CHANNEL,
            .max_connection = CONFIG_RW_AP_MAX_CONN,
            .authmode = sizeof(CONFIG_RW_AP_PASSWORD) > 1 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        },
    };
    memcpy(cfg.ap.ssid, s_ap_ssid, cfg.ap.ssid_len);
    return esp_wifi_set_config(WIFI_IF_AP, &cfg);
}

esp_err_t net_init(void)
{
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "event group");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "event loop");

    s_sta = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_sta, ESP_FAIL, TAG, "sta netif");
    s_ap = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_ap, ESP_FAIL, TAG, "ap netif");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL), TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL), TAG, "ip events");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(ap_configure(), TAG, "ap config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    esp_log_level_set("wifi", ESP_LOG_ERROR);

    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap, &ip);
    ESP_LOGI(TAG, "ap \"%s\" up, http://" IPSTR "/", s_ap_ssid, IP2STR(&ip.ip));
    return ESP_OK;
}

esp_netif_t *net_sta(void)
{
    return s_sta;
}

esp_netif_t *net_ap(void)
{
    return s_ap;
}

const char *net_ap_ssid(void)
{
    return s_ap_ssid;
}

esp_err_t net_connect(const char *ssid, const char *password)
{
    wifi_config_t cfg = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
        },
    };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if (password) {
        strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set config");
    s_last_reason = 0;
    s_reconnect = true;
    return esp_wifi_connect();
}

void net_disconnect(void)
{
    s_reconnect = false;
    s_leaving = net_associated();
    esp_wifi_disconnect();
    xEventGroupClearBits(s_events, NET_ASSOCIATED | NET_GOT_IP);
}

bool net_associated(void)
{
    return xEventGroupGetBits(s_events) & NET_ASSOCIATED;
}

bool net_has_ip(void)
{
    return xEventGroupGetBits(s_events) & NET_GOT_IP;
}

bool net_reconnect_enabled(void)
{
    return s_reconnect;
}

uint8_t net_last_reason(void)
{
    return s_last_reason;
}

const char *net_reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE: return "auth expired";
    case WIFI_REASON_ASSOC_LEAVE: return "left";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "handshake timeout, wrong password?";
    case WIFI_REASON_BEACON_TIMEOUT: return "beacon timeout";
    case WIFI_REASON_NO_AP_FOUND: return "network not found";
    case WIFI_REASON_AUTH_FAIL: return "auth failed";
    case WIFI_REASON_ASSOC_FAIL: return "association failed";
    case WIFI_REASON_CONNECTION_FAIL: return "connection failed";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "no network with compatible security";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "no network in auth mode threshold";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: return "no network in rssi threshold";
    default: return "other";
    }
}

const char *net_auth_str(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "wep";
    case WIFI_AUTH_WPA_PSK: return "wpa";
    case WIFI_AUTH_WPA2_PSK: return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
    case WIFI_AUTH_WPA3_PSK: return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
    case WIFI_AUTH_OWE: return "owe";
    case WIFI_AUTH_WAPI_PSK: return "wapi";
    case WIFI_AUTH_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "enterprise";
    default: return "other";
    }
}

const char *net_band_str(uint8_t channel)
{
    return channel > 14 ? "5" : "2.4";
}
