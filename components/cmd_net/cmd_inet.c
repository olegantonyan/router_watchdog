#include <stdio.h>
#include <inttypes.h>
#include "argtable3/argtable3.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "shell.h"
#include "inet.h"
#include "net.h"
#include "cmd_net_priv.h"

static const char *TAG = "cmd_inet";

static void print_report(const inet_report_t *r)
{
    int64_t now = esp_timer_get_time();
    char d[16];
    printf("state      %s", inet_state_str(r->state));
    if (r->state != INET_UNKNOWN) {
        printf(" for %s", shell_fmt_duration(d, sizeof(d), now - r->state_since_us));
    }
    printf("\n");
    if (r->state == INET_OFFLINE) {
        printf("reason     %s\n", r->reason);
    } else if (r->state == INET_ONLINE && r->fail_streak) {
        printf("failing    %s, %" PRIu32 " of %d rounds before offline\n", r->reason, r->fail_streak, CONFIG_RW_INET_FAIL_ROUNDS);
    }
    if (!r->rounds) {
        printf("last check never\n");
    } else {
        printf("last check %s ago (round %" PRIu32 ")\n", shell_fmt_duration(d, sizeof(d), now - r->last_round_us), r->rounds);
    }
    if (r->running) {
        printf("next check running now\n");
    } else if (!net_has_ip()) {
        printf("next check waiting for wifi\n");
    } else if (r->next_round_us) {
        printf("next check in %s\n", shell_fmt_duration(d, sizeof(d), r->next_round_us > now ? r->next_round_us - now : 0));
    }
    if (!r->count) {
        return;
    }
    printf("\n%-11s %-30s %-6s %s\n", "probe", "target", "result", "time");
    for (size_t i = 0; i < r->count; i++) {
        const inet_probe_t *p = &r->probes[i];
        printf("%-11s %-30s %-6s ", p->name, p->target, p->ok ? "ok" : "fail");
        if (p->ok) {
            printf("%" PRIu32 " ms\n", p->ms);
        } else {
            printf("%s\n", p->error);
        }
    }
}

static int cmd_status(int argc, char **argv)
{
    inet_report_t r;
    inet_report(&r);
    print_report(&r);
    return 0;
}

static struct {
    struct arg_int *timeout;
    struct arg_end *end;
} s_check_args;

static int cmd_check(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_check_args)) {
        return 1;
    }
    int timeout_s = s_check_args.timeout->count ? s_check_args.timeout->ival[0] : 90;
    if (!net_has_ip()) {
        fprintf(stderr, "check: network is down, see 'wifi connect'\n");
        return 1;
    }
    inet_report_t r;
    inet_report(&r);
    uint32_t target = r.rounds + (r.running ? 2 : 1);
    inet_check_now();
    printf("checking (Ctrl+C to abort)\n");
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_s * 1000000;
    while (r.rounds < target) {
        if (shell_getc(200) == SHELL_CTRL_C) {
            fprintf(stderr, "check: aborted\n");
            return 1;
        }
        if (!net_has_ip()) {
            fprintf(stderr, "check: network went down\n");
            return 1;
        }
        if (esp_timer_get_time() > deadline) {
            fprintf(stderr, "check: timeout after %d s\n", timeout_s);
            return 1;
        }
        inet_report(&r);
    }
    print_report(&r);
    return r.state == INET_ONLINE ? 0 : 1;
}

static const shell_subcmd_t s_subcmds[] = {
    { .name = "status", .help = "Internet state and last probe results", .func = cmd_status },
    { .name = "check", .help = "Run all probes now and show the results", .func = cmd_check, .argtable = &s_check_args },
};

static int cmd_inet(int argc, char **argv)
{
    return shell_subcmd_run("inet", s_subcmds, sizeof(s_subcmds) / sizeof(s_subcmds[0]), argc, argv);
}

esp_err_t cmd_inet_register(void)
{
    s_check_args.timeout = arg_int0("t", "timeout", "<sec>", "wait for the result, default 90");
    s_check_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "inet",
        .help = "Internet check: status | check",
        .hint = "<command> [args]",
        .func = cmd_inet,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register inet");
    return ESP_OK;
}
