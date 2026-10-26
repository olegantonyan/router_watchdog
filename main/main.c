#include "esp_err.h"
#include "shell.h"
#include "cmd_system.h"

void app_main(void)
{
    ESP_ERROR_CHECK(shell_init());
    ESP_ERROR_CHECK(cmd_system_register());
    ESP_ERROR_CHECK(shell_start());
}
