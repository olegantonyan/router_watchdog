#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#define CLOCK_MAX_SERVERS 3

esp_err_t clock_init(void);
bool clock_is_set(void);

uint32_t clock_sync_count(void);
time_t clock_last_sync(void);
uint32_t clock_sync_interval_s(void);
void clock_resync(void);

const char *clock_server(int idx);
uint8_t clock_server_reach(int idx);
esp_err_t clock_set_server(int idx, const char *host);

const char *clock_tz(void);
esp_err_t clock_set_tz(const char *posix);
