#include "esp_check.h"
#include "esp_console.h"
#include "shell.h"

static const char *TAG = "shell";

static esp_console_repl_t *s_repl;

esp_err_t shell_init(void)
{
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "rw> ";
    cfg.task_stack_size = 8192;
    cfg.max_cmdline_length = 256;
    cfg.max_cmdline_args = 16;
    cfg.max_history_len = 32;

    ESP_RETURN_ON_ERROR(esp_console_new_repl_stdio(&cfg, &s_repl), TAG, "create REPL");
    return esp_console_register_help_command();
}

esp_err_t shell_start(void)
{
    ESP_RETURN_ON_FALSE(s_repl, ESP_ERR_INVALID_STATE, TAG, "shell_init not called");
    return esp_console_start_repl(s_repl);
}
