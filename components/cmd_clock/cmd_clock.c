#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include "argtable3/argtable3.h"
#include "esp_check.h"
#include "esp_console.h"
#include "clock.h"
#include "shell.h"
#include "cmd_clock.h"

static const char *TAG = "cmd_clock";

static const struct {
    const char *name;
    const char *posix;
} s_zones[] = {
    { "UTC", "UTC0" },
    { "Europe/Belgrade", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Moscow", "MSK-3" },
    { "America/New_York", "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Chicago", "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Denver", "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0" },
    { "Asia/Tokyo", "JST-9" },
};

static struct {
    struct arg_lit *utc;
    struct arg_str *set;
    struct arg_str *format;
    struct arg_end *end;
} s_date_args;

static struct {
    struct arg_lit *list;
    struct arg_str *zone;
    struct arg_end *end;
} s_tz_args;

static struct {
    struct arg_int *timeout;
    struct arg_end *end;
} s_sync_args;

static struct {
    struct arg_int *index;
    struct arg_str *host;
    struct arg_end *end;
} s_server_args;

static bool parse_datetime(const char *s, struct tm *tm)
{
    int y, mo, d, h, mi, sec = 0;
    char sep;
    int n = sscanf(s, "%d-%d-%d%c%d:%d:%d", &y, &mo, &d, &sep, &h, &mi, &sec);
    if ((n != 6 && n != 7) || (sep != ' ' && sep != 'T') || mo < 1 || mo > 12 || d < 1 || d > 31
        || h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 59 || y < 2025 || y > 2099) {
        return false;
    }
    *tm = (struct tm){ .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_sec = sec, .tm_isdst = -1 };
    return true;
}

static int cmd_date(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_date_args)) {
        return 1;
    }
    if (s_date_args.set->count) {
        struct tm tm;
        if (!parse_datetime(s_date_args.set->sval[0], &tm)) {
            fprintf(stderr, "date: invalid time '%s', use \"YYYY-MM-DD HH:MM[:SS]\" (local time, year 2025-2099)\n", s_date_args.set->sval[0]);
            return 1;
        }
        struct timeval tv = { .tv_sec = mktime(&tm) };
        if (settimeofday(&tv, NULL) != 0) {
            fprintf(stderr, "date: settimeofday failed\n");
            return 1;
        }
    }

    const char *fmt = "%a %b %e %H:%M:%S %Z %Y";
    if (s_date_args.format->count) {
        fmt = s_date_args.format->sval[0];
        if (fmt[0] != '+') {
            fprintf(stderr, "date: format must start with '+', e.g. date +%%Y-%%m-%%d\n");
            return 1;
        }
        fmt++;
    }
    time_t now = time(NULL);
    struct tm tm;
    if (s_date_args.utc->count) {
        gmtime_r(&now, &tm);
    } else {
        localtime_r(&now, &tm);
    }
    char buf[128];
    size_t n = strftime(buf, sizeof(buf), fmt, &tm);
    if (n == 0 && fmt[0]) {
        fprintf(stderr, "date: format too long\n");
        return 1;
    }
    if (s_date_args.utc->count && !s_date_args.format->count) {
        strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S UTC %Y", &tm);
    }
    printf("%s\n", buf);
    if (!clock_is_set()) {
        fprintf(stderr, "date: clock not set yet, waiting for NTP (see 'ntp status')\n");
    }
    return 0;
}

static const char *zone_posix(const char *name)
{
    for (size_t i = 0; i < sizeof(s_zones) / sizeof(s_zones[0]); i++) {
        if (strcasecmp(name, s_zones[i].name) == 0) {
            return s_zones[i].posix;
        }
    }
    return NULL;
}

static int cmd_tz(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_tz_args)) {
        return 1;
    }
    if (s_tz_args.list->count) {
        for (size_t i = 0; i < sizeof(s_zones) / sizeof(s_zones[0]); i++) {
            printf("%-20s %s\n", s_zones[i].name, s_zones[i].posix);
        }
        printf("or any POSIX TZ string, e.g. tz \"<+04>-4\"\n");
        return 0;
    }
    if (s_tz_args.zone->count) {
        const char *zone = s_tz_args.zone->sval[0];
        const char *posix = zone_posix(zone);
        if (clock_set_tz(posix ? posix : zone) != ESP_OK) {
            fprintf(stderr, "tz: invalid zone '%s', see 'tz -l'\n", zone);
            return 1;
        }
    }
    time_t now = time(NULL);
    struct tm tm;
    char abbr[16], offset[8];
    localtime_r(&now, &tm);
    strftime(abbr, sizeof(abbr), "%Z", &tm);
    strftime(offset, sizeof(offset), "%z", &tm);
    printf("%s (now %s, UTC%.3s:%s)\n", clock_tz(), abbr, offset, offset + 3);
    return 0;
}

static void print_ago(time_t t)
{
    long ago = (long)(time(NULL) - t);
    if (ago < 60) {
        printf("%lds ago", ago);
    } else if (ago < 3600) {
        printf("%ldm ago", ago / 60);
    } else {
        printf("%ldh %ldm ago", ago / 3600, ago / 60 % 60);
    }
}

static int cmd_ntp_status(int argc, char **argv)
{
    uint32_t count = clock_sync_count();
    if (count) {
        time_t last = clock_last_sync();
        struct tm tm;
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime_r(&last, &tm));
        printf("status     synced %lu time%s, last %s (", (unsigned long)count, count == 1 ? "" : "s", buf);
        print_ago(last);
        printf(")\n");
    } else {
        printf("status     not synced since boot%s\n", clock_is_set() ? ", clock kept from before reset" : "");
    }
    printf("interval   %lu s\n", (unsigned long)clock_sync_interval_s());
    for (int i = 0; i < CLOCK_MAX_SERVERS; i++) {
        const char *host = clock_server(i);
        if (host) {
            printf("server %d   %-24s reach 0x%02x\n", i + 1, host, clock_server_reach(i));
        } else {
            printf("server %d   -\n", i + 1);
        }
    }
    return 0;
}

static int cmd_ntp_sync(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_sync_args)) {
        return 1;
    }
    int timeout_s = s_sync_args.timeout->count ? s_sync_args.timeout->ival[0] : 10;
    uint32_t before = clock_sync_count();
    clock_resync();
    printf("syncing (Ctrl+C to abort)\n");
    for (int waited = 0; clock_sync_count() == before; waited += 100) {
        if (waited >= timeout_s * 1000) {
            fprintf(stderr, "ntp: no reply after %d s; check 'wifi status' and 'ntp status'\n", timeout_s);
            return 1;
        }
        if (shell_getc(100) == SHELL_CTRL_C) {
            fprintf(stderr, "ntp: aborted\n");
            return 1;
        }
    }
    time_t now = time(NULL);
    struct tm tm;
    char buf[48];
    strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S %Z %Y", localtime_r(&now, &tm));
    printf("%s\n", buf);
    return 0;
}

static int cmd_ntp_server(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_server_args)) {
        return 1;
    }
    int idx = s_server_args.index->ival[0];
    const char *host = s_server_args.host->sval[0];
    if (idx < 1 || idx > CLOCK_MAX_SERVERS) {
        fprintf(stderr, "ntp: server index must be 1-%d\n", CLOCK_MAX_SERVERS);
        return 1;
    }
    esp_err_t err = clock_set_server(idx - 1, strcmp(host, "none") == 0 ? NULL : host);
    if (err != ESP_OK) {
        fprintf(stderr, "ntp: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static const shell_subcmd_t s_ntp_subcmds[] = {
    { .name = "status", .help = "Sync state, servers and reachability", .func = cmd_ntp_status },
    { .name = "sync", .help = "Sync now and wait", .func = cmd_ntp_sync, .argtable = &s_sync_args },
    { .name = "server", .help = "Replace a server (RAM only); 'none' clears it", .func = cmd_ntp_server, .argtable = &s_server_args },
};

static int cmd_ntp(int argc, char **argv)
{
    return shell_subcmd_run("ntp", s_ntp_subcmds, sizeof(s_ntp_subcmds) / sizeof(s_ntp_subcmds[0]), argc, argv);
}

esp_err_t cmd_clock_register(void)
{
    s_date_args.utc = arg_lit0("u", "utc", "show UTC instead of local time");
    s_date_args.set = arg_str0("s", "set", "<\"YYYY-MM-DD HH:MM[:SS]\">", "set local time");
    s_date_args.format = arg_str0(NULL, NULL, "<+format>", "strftime format, e.g. +%Y-%m-%dT%H:%M:%S%z");
    s_date_args.end = arg_end(3);
    s_tz_args.list = arg_lit0("l", "list", "list named zones");
    s_tz_args.zone = arg_str0(NULL, NULL, "<zone>", "named zone or POSIX TZ string");
    s_tz_args.end = arg_end(2);
    s_sync_args.timeout = arg_int0("t", "timeout", "<sec>", "wait, default 10");
    s_sync_args.end = arg_end(2);
    s_server_args.index = arg_int1(NULL, NULL, "<1-3>", "server slot");
    s_server_args.host = arg_str1(NULL, NULL, "<host|none>", "hostname or IP");
    s_server_args.end = arg_end(2);

    const esp_console_cmd_t cmds[] = {
        { .command = "date", .help = "Show or set date and time", .func = cmd_date, .argtable = &s_date_args },
        { .command = "tz", .help = "Show or set the timezone (RAM only)", .func = cmd_tz, .argtable = &s_tz_args },
        { .command = "ntp", .help = "NTP time sync: status | sync | server", .hint = "<command> [args]", .func = cmd_ntp },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), TAG, "register %s", cmds[i].command);
    }
    return ESP_OK;
}
