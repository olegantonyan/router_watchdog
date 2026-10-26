#include "esp_err.h"
#include "nvs_flash.h"
#include "shell.h"
#include "cmd_system.h"
#include "cmd_net.h"

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
    ESP_ERROR_CHECK(shell_init());
    ESP_ERROR_CHECK(cmd_system_register());
    ESP_ERROR_CHECK(cmd_net_register());
    ESP_ERROR_CHECK(shell_start());
}
