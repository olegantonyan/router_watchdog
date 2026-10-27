#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define INET_MAX_PROBES 7

typedef enum { INET_UNKNOWN, INET_ONLINE, INET_OFFLINE } inet_state_t;

typedef struct {
    const char *name;
    char target[40];
    bool http;
    bool ok;
    uint32_t ms;
    char error[24];
} inet_probe_t;

typedef struct {
    inet_state_t state;
    int64_t state_since_us;
    int64_t last_round_us;
    int64_t next_round_us;
    uint32_t rounds;
    uint32_t fail_streak;
    bool running;
    char reason[40];
    size_t count;
    inet_probe_t probes[INET_MAX_PROBES];
} inet_report_t;

esp_err_t inet_start(void);
inet_state_t inet_state(void);
const char *inet_state_str(inet_state_t state);
void inet_report(inet_report_t *out);
void inet_check_now(void);
