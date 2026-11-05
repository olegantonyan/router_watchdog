#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "inet.h"
#include "net.h"
#include "relay.h"
#include "watchdog.h"

#define US_PER_S 1000000LL

static const char *TAG = "watchdog";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static watchdog_status_t s_st;
static int64_t s_offline_since, s_noip_since;
static bool s_manual;

static void power_cycle(const char *reason)
{
    taskENTER_CRITICAL(&s_lock);
    s_st.cycling = true;
    strlcpy(s_st.last_reason, reason, sizeof(s_st.last_reason));
    taskEXIT_CRITICAL(&s_lock);

    ESP_LOGW(TAG, "power cycling router (relay %d off for %d ms): %s", s_st.relay, CONFIG_RW_WD_PULSE_MS, reason);
    relay_set(s_st.relay, true);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_RW_WD_PULSE_MS));
    if (relay_set(s_st.relay, false) != ESP_OK) {
        ESP_LOGE(TAG, "relay %d failed to switch back", s_st.relay);
    }

    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    s_st.cycling = false;
    s_st.cycles++;
    s_st.last_cycle_us = now;
    s_st.holdoff_until_us = now + s_st.next_holdoff_s * US_PER_S;
    uint32_t next = s_st.next_holdoff_s * 2;
    s_st.next_holdoff_s = next > CONFIG_RW_WD_GRACE_MAX_S ? CONFIG_RW_WD_GRACE_MAX_S : next;
    s_offline_since = 0;
    s_noip_since = 0;
    taskEXIT_CRITICAL(&s_lock);
}

static const char *due(int64_t now)
{
    if (s_manual) {
        s_manual = false;
        return "manual";
    }
    if (!s_st.enabled || now < s_st.holdoff_until_us) {
        return NULL;
    }
    if (s_offline_since && now - s_offline_since >= CONFIG_RW_WD_OFFLINE_S * US_PER_S) {
        return "internet offline";
    }
    if (CONFIG_RW_WD_NO_WIFI_S && s_noip_since && now - s_noip_since >= CONFIG_RW_WD_NO_WIFI_S * US_PER_S) {
        return "no wifi";
    }
    return NULL;
}

static void watchdog_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        inet_state_t inet = inet_state();
        bool noip = !net_has_ip() && net_reconnect_enabled();

        taskENTER_CRITICAL(&s_lock);
        if (inet == INET_ONLINE) {
            s_st.next_holdoff_s = CONFIG_RW_WD_GRACE_S;
        }
        s_offline_since = inet != INET_OFFLINE ? 0 : s_offline_since ? s_offline_since : now;
        s_noip_since = !noip ? 0 : s_noip_since ? s_noip_since : now;
        const char *reason = due(now);
        taskEXIT_CRITICAL(&s_lock);

        if (reason) {
            power_cycle(reason);
        }
    }
}

esp_err_t watchdog_start(void)
{
    ESP_RETURN_ON_FALSE(relay_gpio(CONFIG_RW_WD_RELAY) >= 0, ESP_ERR_INVALID_STATE, TAG, "relay %d not connected", CONFIG_RW_WD_RELAY);
    s_st = (watchdog_status_t){
        .enabled = true,
        .relay = CONFIG_RW_WD_RELAY,
        .next_holdoff_s = CONFIG_RW_WD_GRACE_S,
    };
    ESP_RETURN_ON_FALSE(xTaskCreate(watchdog_task, "watchdog", 3072, NULL, 2, &s_task) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void watchdog_enable(bool on)
{
    taskENTER_CRITICAL(&s_lock);
    s_st.enabled = on;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "%s", on ? "enabled" : "disabled");
}

void watchdog_cycle_now(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_manual = true;
    taskEXIT_CRITICAL(&s_lock);
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void watchdog_status(watchdog_status_t *out)
{
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    *out = s_st;
    if (s_offline_since) {
        out->pending = "internet offline";
        out->pending_for_us = now - s_offline_since;
        out->pending_limit_s = CONFIG_RW_WD_OFFLINE_S;
    } else if (CONFIG_RW_WD_NO_WIFI_S && s_noip_since) {
        out->pending = "no wifi";
        out->pending_for_us = now - s_noip_since;
        out->pending_limit_s = CONFIG_RW_WD_NO_WIFI_S;
    }
    taskEXIT_CRITICAL(&s_lock);
}
