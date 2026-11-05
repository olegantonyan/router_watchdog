#include <stdio.h>
#include "argtable3/argtable3.h"
#include "esp_check.h"
#include "esp_console.h"
#include "relay.h"
#include "shell.h"
#include "cmd_relay.h"

static const char *TAG = "cmd_relay";

static struct {
    struct arg_int *n;
    struct arg_end *end;
} s_on_args, s_off_args;

static int cmd_status(int argc, char **argv)
{
    printf("%-5s %-4s %s\n", "relay", "gpio", "state");
    for (int n = 1; n <= RELAY_COUNT; n++) {
        int gpio = relay_gpio(n);
        if (gpio < 0) {
            printf("%-5d %-4s %s\n", n, "-", "not connected");
        } else {
            printf("%-5d %-4d %s\n", n, gpio, relay_get(n) ? "on" : "off");
        }
    }
    return 0;
}

static int set(int argc, char **argv, void *args, struct arg_int *n, bool on)
{
    if (!shell_args_ok(argc, argv, args)) {
        return 1;
    }
    int num = n->ival[0];
    if (num < 1 || num > RELAY_COUNT) {
        fprintf(stderr, "relay: number must be 1-%d\n", RELAY_COUNT);
        return 1;
    }
    if (relay_gpio(num) < 0) {
        fprintf(stderr, "relay: %d is not connected (idf.py menuconfig -> Router watchdog relays)\n", num);
        return 1;
    }
    if (relay_set(num, on) != ESP_OK) {
        fprintf(stderr, "relay: failed to switch %d\n", num);
        return 1;
    }
    return 0;
}

static int cmd_on(int argc, char **argv)
{
    return set(argc, argv, &s_on_args, s_on_args.n, true);
}

static int cmd_off(int argc, char **argv)
{
    return set(argc, argv, &s_off_args, s_off_args.n, false);
}

static const shell_subcmd_t s_subcmds[] = {
    { .name = "status", .help = "GPIO and state of every relay", .func = cmd_status },
    { .name = "on", .help = "Switch a relay on", .func = cmd_on, .argtable = &s_on_args },
    { .name = "off", .help = "Switch a relay off", .func = cmd_off, .argtable = &s_off_args },
};

static int cmd_relay(int argc, char **argv)
{
    return shell_subcmd_run("relay", s_subcmds, sizeof(s_subcmds) / sizeof(s_subcmds[0]), argc, argv);
}

esp_err_t cmd_relay_register(void)
{
    s_on_args.n = arg_int1(NULL, NULL, "<1-8>", "relay number");
    s_on_args.end = arg_end(2);
    s_off_args.n = arg_int1(NULL, NULL, "<1-8>", "relay number");
    s_off_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "relay",
        .help = "Relays: status | on | off",
        .hint = "<command> [args]",
        .func = cmd_relay,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register relay");
    return ESP_OK;
}
