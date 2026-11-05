#include <stdio.h>
#include "argtable3/argtable3.h"
#include "esp_check.h"
#include "esp_console.h"
#include "led.h"
#include "shell.h"
#include "cmd_led.h"

#define MIN_PERIOD_MS 40

static const char *TAG = "cmd_led";

static struct {
    struct arg_int *period;
    struct arg_end *end;
} s_blink_args, s_breathe_args;

static struct {
    struct arg_int *percent;
    struct arg_end *end;
} s_brightness_args;

static int cmd_status(int argc, char **argv)
{
    led_state_t st;
    led_get(&st);
    printf("mode       %s\n", st.auto_mode ? "auto (network status)" : "manual");
    printf("effect     %s\n", led_effect_name(st.effect));
    if (st.effect == LED_BLINK || st.effect == LED_BREATHE) {
        printf("period     %lu ms\n", (unsigned long)st.period_ms);
    }
    printf("brightness %u%%\n", st.brightness);
    return 0;
}

static int cmd_auto(int argc, char **argv)
{
    if (led_auto()) {
        return 0;
    }
    fprintf(stderr, "led: no auto mode handler installed\n");
    return 1;
}

static int cmd_on(int argc, char **argv)
{
    led_set(LED_ON, 0);
    return 0;
}

static int cmd_off(int argc, char **argv)
{
    led_set(LED_OFF, 0);
    return 0;
}

static int set_periodic(int argc, char **argv, void *args, struct arg_int *period, led_effect_t effect, uint32_t def_period)
{
    if (!shell_args_ok(argc, argv, args)) {
        return 1;
    }
    if (period->count && period->ival[0] < MIN_PERIOD_MS) {
        fprintf(stderr, "led: period must be >= %d ms\n", MIN_PERIOD_MS);
        return 1;
    }
    led_set(effect, period->count ? (uint32_t)period->ival[0] : def_period);
    return 0;
}

static int cmd_blink(int argc, char **argv)
{
    return set_periodic(argc, argv, &s_blink_args, s_blink_args.period, LED_BLINK, 500);
}

static int cmd_breathe(int argc, char **argv)
{
    return set_periodic(argc, argv, &s_breathe_args, s_breathe_args.period, LED_BREATHE, 3000);
}

static int cmd_brightness(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_brightness_args)) {
        return 1;
    }
    int pct = s_brightness_args.percent->ival[0];
    if (pct < 0 || pct > 100) {
        fprintf(stderr, "led: brightness must be 0-100\n");
        return 1;
    }
    led_set_brightness(pct);
    return 0;
}

static const shell_subcmd_t s_subcmds[] = {
    { .name = "status", .help = "Current mode, effect and brightness", .func = cmd_status },
    { .name = "auto", .help = "Show network status (default)", .func = cmd_auto },
    { .name = "on", .help = "Turn on", .func = cmd_on },
    { .name = "off", .help = "Turn off", .func = cmd_off },
    { .name = "blink", .help = "Blink, default period 500 ms", .func = cmd_blink, .argtable = &s_blink_args },
    { .name = "breathe", .help = "Fade in and out, default period 3000 ms", .func = cmd_breathe, .argtable = &s_breathe_args },
    { .name = "brightness", .help = "Brightness, default 100", .func = cmd_brightness, .argtable = &s_brightness_args },
};

static int cmd_led(int argc, char **argv)
{
    return shell_subcmd_run("led", s_subcmds, sizeof(s_subcmds) / sizeof(s_subcmds[0]), argc, argv);
}

esp_err_t cmd_led_register(void)
{
    s_blink_args.period = arg_int0("p", "period", "<ms>", "full on+off cycle");
    s_blink_args.end = arg_end(2);
    s_breathe_args.period = arg_int0("p", "period", "<ms>", "full fade in+out cycle");
    s_breathe_args.end = arg_end(2);
    s_brightness_args.percent = arg_int1(NULL, NULL, "<0-100>", "percent");
    s_brightness_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "led",
        .help = "Status LED: status | auto | on | off | blink | breathe | brightness",
        .hint = "<command> [args]",
        .func = cmd_led,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register led");
    return ESP_OK;
}
