#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool enabled;
    bool cycling;
    int relay;
    const char *pending;
    int64_t pending_for_us;
    uint32_t pending_limit_s;
    uint32_t cycles;
    int64_t last_cycle_us;
    char last_reason[32];
    int64_t holdoff_until_us;
    uint32_t next_holdoff_s;
} watchdog_status_t;

esp_err_t watchdog_start(void);
void watchdog_enable(bool on);
void watchdog_cycle_now(void);
void watchdog_status(watchdog_status_t *out);
