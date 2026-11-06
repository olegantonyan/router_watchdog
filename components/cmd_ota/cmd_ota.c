#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "argtable3/argtable3.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota.h"
#include "shell.h"
#include "cmd_ota.h"

#define CHUNK 4096
#define MAX_REDIRECTS 3

static const char *TAG = "cmd_ota";

static struct {
    struct arg_lit *no_reboot;
    struct arg_str *url;
    struct arg_end *end;
} s_update_args;

static int cmd_status(int argc, char **argv)
{
    ota_status_t st;
    ota_status(&st);
    printf("running    %s on %s\n", st.version, st.running);
    if (st.pending_verify) {
        printf("state      pending verify, marked valid in %ds; a reset before that rolls back\n", st.valid_in_s);
    } else {
        printf("state      %s\n", ota_state_str(&st));
    }
    if (st.updating) {
        if (st.total) {
            printf("progress   %zu of %zu KB (%d%%)\n", st.written / 1024, st.total / 1024, (int)(st.written * 100 / st.total));
        } else {
            printf("progress   %zu KB\n", st.written / 1024);
        }
    }
    printf("other slot %s: %s\n", st.other ? st.other : "-", st.other_version[0] ? st.other_version : "empty");
    printf("rollback   %s\n", st.rollback_possible ? "available (ota rollback)" : "not available");
    return 0;
}

static esp_http_client_handle_t open_url(const char *url, int64_t *len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        fprintf(stderr, "ota: invalid url\n");
        return NULL;
    }
    for (int i = 0; i <= MAX_REDIRECTS; i++) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            fprintf(stderr, "ota: connect: %s\n", esp_err_to_name(err));
            break;
        }
        *len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            return client;
        }
        bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        if (!redirect || i == MAX_REDIRECTS) {
            fprintf(stderr, "ota: HTTP %d\n", status);
            break;
        }
        esp_http_client_set_redirection(client);
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    return NULL;
}

static const char *download(esp_http_client_handle_t client, size_t total)
{
    char *buf = malloc(CHUNK);
    if (!buf) {
        return "out of memory";
    }
    const char *err = NULL;
    size_t done = 0;
    int last_step = 0;
    for (;;) {
        if (shell_getc(0) == SHELL_CTRL_C) {
            err = "aborted";
            break;
        }
        int n = esp_http_client_read(client, buf, CHUNK);
        if (n < 0) {
            err = "download failed";
            break;
        }
        if (n == 0) {
            if (total && done < total) {
                err = "download incomplete";
            }
            break;
        }
        if ((err = ota_write(buf, n))) {
            break;
        }
        done += n;
        int step = total ? done * 10 / total : done / (128 * 1024);
        if (step != last_step) {
            last_step = step;
            if (total) {
                printf("%3d%% %zu KB\n", step * 10, done / 1024);
            } else {
                printf("%zu KB\n", done / 1024);
            }
        }
    }
    free(buf);
    return err;
}

static int cmd_update(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_update_args)) {
        return 1;
    }
    int64_t len = 0;
    esp_http_client_handle_t client = open_url(s_update_args.url->sval[0], &len);
    if (!client) {
        return 1;
    }
    size_t total = len > 0 ? len : 0;
    const char *err = ota_begin(total);
    if (!err) {
        printf("downloading %zu KB\n", total / 1024);
        err = download(client, total);
        const char *fin = ota_finish(!err);
        err = err ? err : fin;
    }
    esp_http_client_cleanup(client);
    if (err) {
        fprintf(stderr, "ota: %s\n", err);
        return 1;
    }

    ota_status_t st;
    ota_status(&st);
    if (s_update_args.no_reboot->count) {
        printf("installed %s on %s, boots on next reboot\n", st.other_version, st.other);
        return 0;
    }
    printf("installed %s on %s, rebooting\n", st.other_version, st.other);
    fflush(stdout);
    ota_reboot(1000);
    return 0;
}

static int cmd_rollback(int argc, char **argv)
{
    ota_status_t st;
    ota_status(&st);
    if (st.updating || !st.rollback_possible) {
        fprintf(stderr, "ota: %s\n", st.updating ? "update in progress" : "no previous firmware to roll back to");
        return 1;
    }
    printf("rolling back to %s on %s, rebooting\n", st.other_version[0] ? st.other_version : "previous firmware", st.other);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_err_t err = ota_rollback();
    fprintf(stderr, "ota: rollback failed: %s\n", esp_err_to_name(err));
    return 1;
}

static const shell_subcmd_t s_subcmds[] = {
    { .name = "status", .help = "Running firmware, verify state, other slot, update progress", .func = cmd_status },
    { .name = "update", .help = "Download firmware from an http(s) URL, install it and reboot", .func = cmd_update, .argtable = &s_update_args },
    { .name = "rollback", .help = "Reboot into the previous firmware", .func = cmd_rollback },
};

static int cmd_ota(int argc, char **argv)
{
    return shell_subcmd_run("ota", s_subcmds, sizeof(s_subcmds) / sizeof(s_subcmds[0]), argc, argv);
}

esp_err_t cmd_ota_register(void)
{
    s_update_args.no_reboot = arg_lit0("n", NULL, "don't reboot after installing");
    s_update_args.url = arg_str1(NULL, NULL, "<url>", "firmware .bin, e.g. http://192.168.4.2:8000/router_watchdog.bin");
    s_update_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "ota",
        .help = "Firmware update: status | update | rollback",
        .hint = "<command> [args]",
        .func = cmd_ota,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register ota");
    return ESP_OK;
}
