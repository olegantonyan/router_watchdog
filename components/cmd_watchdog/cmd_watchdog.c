#include <stdio.h>
#include <inttypes.h>
#include "esp_check.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "relay.h"
#include "shell.h"
#include "watchdog.h"
#include "cmd_watchdog.h"

#define US_PER_S 1000000LL

static const char *TAG = "cmd_watchdog";

static bool running(const watchdog_status_t *st)
{
    if (st->relay) {
        return true;
    }
    fprintf(stderr, "watchdog: not running, relay %d is not connected\n", CONFIG_RW_WD_RELAY);
    return false;
}

static int cmd_status(int argc, char **argv)
{
    watchdog_status_t st;
    watchdog_status(&st);
    if (!running(&st)) {
        return 1;
    }
    int64_t now = esp_timer_get_time();
    char d[16], d2[16];
    printf("state      ");
    if (st.cycling) {
        printf("power cycling now\n");
    } else if (st.holdoff_until_us > now) {
        printf("%s, waiting %s after %s\n", st.enabled ? "enabled" : "disabled",
               shell_fmt_duration(d, sizeof(d), st.holdoff_until_us - now), st.cycles ? "last cycle" : "boot");
    } else {
        printf("%s\n", st.enabled ? "enabled" : "disabled");
    }
    printf("relay      %d (GPIO%d), off for %d ms\n", st.relay, relay_gpio(st.relay), CONFIG_RW_WD_PULSE_MS);
    printf("triggers   internet offline %s", shell_fmt_duration(d, sizeof(d), CONFIG_RW_WD_OFFLINE_S * US_PER_S));
    if (CONFIG_RW_WD_NO_WIFI_S) {
        printf(", no wifi %s", shell_fmt_duration(d, sizeof(d), CONFIG_RW_WD_NO_WIFI_S * US_PER_S));
    }
    printf("\n");
    if (st.pending) {
        printf("pending    %s for %s of %s\n", st.pending, shell_fmt_duration(d, sizeof(d), st.pending_for_us),
               shell_fmt_duration(d2, sizeof(d2), st.pending_limit_s * US_PER_S));
    } else {
        printf("pending    none\n");
    }
    if (st.cycles) {
        printf("cycles     %" PRIu32 ", last %s ago (%s)\n", st.cycles, shell_fmt_duration(d, sizeof(d), now - st.last_cycle_us), st.last_reason);
    } else {
        printf("cycles     0\n");
    }
    printf("next wait  %s after a cycle\n", shell_fmt_duration(d, sizeof(d), st.next_holdoff_s * US_PER_S));
    return 0;
}

static int cmd_cycle(int argc, char **argv)
{
    watchdog_status_t st;
    watchdog_status(&st);
    if (!running(&st)) {
        return 1;
    }
    if (st.cycling) {
        fprintf(stderr, "watchdog: power cycle already running\n");
        return 1;
    }
    watchdog_cycle_now();
    printf("power cycling: relay %d on for %d ms\n", st.relay, CONFIG_RW_WD_PULSE_MS);
    return 0;
}

static int set_enabled(bool on)
{
    watchdog_status_t st;
    watchdog_status(&st);
    if (!running(&st)) {
        return 1;
    }
    watchdog_enable(on);
    return 0;
}

static int cmd_enable(int argc, char **argv)
{
    return set_enabled(true);
}

static int cmd_disable(int argc, char **argv)
{
    return set_enabled(false);
}

static const shell_subcmd_t s_subcmds[] = {
    { .name = "status", .help = "State, triggers, pending condition, power cycle history", .func = cmd_status },
    { .name = "cycle", .help = "Power cycle the router now", .func = cmd_cycle },
    { .name = "enable", .help = "Power cycle automatically (default)", .func = cmd_enable },
    { .name = "disable", .help = "Stop automatic power cycles until enable or reboot", .func = cmd_disable },
};

static int cmd_watchdog(int argc, char **argv)
{
    return shell_subcmd_run("watchdog", s_subcmds, sizeof(s_subcmds) / sizeof(s_subcmds[0]), argc, argv);
}

esp_err_t cmd_watchdog_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "watchdog",
        .help = "Router power cycle: status | cycle | enable | disable",
        .hint = "<command>",
        .func = cmd_watchdog,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register watchdog");
    return ESP_OK;
}
