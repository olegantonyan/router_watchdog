#include "esp_check.h"
#include "cmd_net.h"
#include "cmd_net_priv.h"

static const char *TAG = "cmd_net";

esp_err_t cmd_net_register(void)
{
    ESP_RETURN_ON_ERROR(cmd_wifi_register(), TAG, "wifi commands");
    ESP_RETURN_ON_ERROR(cmd_ap_register(), TAG, "ap commands");
    ESP_RETURN_ON_ERROR(cmd_ping_register(), TAG, "ping command");
    ESP_RETURN_ON_ERROR(cmd_nc_register(), TAG, "nc command");
    return ESP_OK;
}
