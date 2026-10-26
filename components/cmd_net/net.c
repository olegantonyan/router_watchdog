#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "net_priv.h"

#define NET_ASSOCIATED BIT0
#define NET_GOT_IP     BIT1

static const char *TAG = "net";

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta;
static volatile bool s_reconnect;
static volatile uint8_t s_last_reason;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_last_reason = 0;
        xEventGroupSetBits(s_events, NET_ASSOCIATED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = data;
        bool self_initiated = e->reason == WIFI_REASON_ASSOC_LEAVE || e->reason == WIFI_REASON_STA_LEAVING;
        if (xEventGroupGetBits(s_events) & NET_ASSOCIATED) {
            ESP_LOGW(TAG, "disconnected: %s (%d)", net_reason_str(e->reason), e->reason);
        }
        xEventGroupClearBits(s_events, NET_ASSOCIATED | NET_GOT_IP);
        if (s_reconnect && !self_initiated) {
            s_last_reason = e->reason;
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, NET_GOT_IP);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(s_events, NET_GOT_IP);
    }
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

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL), TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL), TAG, "ip events");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    esp_log_level_set("wifi", ESP_LOG_WARN);
    return ESP_OK;
}

esp_netif_t *net_sta(void)
{
    return s_sta;
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
