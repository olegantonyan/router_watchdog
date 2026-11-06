#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "clock.h"
#include "inet.h"
#include "led.h"
#include "relay.h"
#include "watchdog.h"
#include "net.h"
#include "web.h"
#include "telnet.h"
#include "shell.h"
#include "cmd_system.h"
#include "cmd_net.h"
#include "cmd_led.h"
#include "cmd_relay.h"
#include "cmd_watchdog.h"
#include "cmd_clock.h"
#include "status_led.h"

static const char *TAG = "main";

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    nvs_init();
    if (relay_init() != ESP_OK) {
        ESP_LOGE(TAG, "relays failed to start, continuing without them");
    }
    if (led_init() == ESP_OK) {
        led_set_auto(status_led_pattern);
    } else {
        ESP_LOGE(TAG, "led failed to start, continuing without it");
    }
    ESP_ERROR_CHECK(net_init());
    if (clock_init() != ESP_OK) {
        ESP_LOGE(TAG, "ntp failed to start, continuing without it");
    }
    if (inet_start() != ESP_OK) {
        ESP_LOGE(TAG, "internet check failed to start, continuing without it");
    }
    if (watchdog_start() != ESP_OK) {
        ESP_LOGE(TAG, "watchdog failed to start, router won't be power cycled");
    }
    ESP_ERROR_CHECK(shell_init());
    ESP_ERROR_CHECK(cmd_system_register());
    ESP_ERROR_CHECK(cmd_net_register());
    ESP_ERROR_CHECK(cmd_led_register());
    ESP_ERROR_CHECK(cmd_relay_register());
    ESP_ERROR_CHECK(cmd_watchdog_register());
    ESP_ERROR_CHECK(cmd_clock_register());
    if (web_start() != ESP_OK) {
        ESP_LOGE(TAG, "web server failed to start, continuing without it");
    }
    if (telnet_start() != ESP_OK) {
        ESP_LOGE(TAG, "telnet failed to start, shell only on UART");
    }
    ESP_ERROR_CHECK(shell_start());
}
