#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define RELAY_COUNT 8

esp_err_t relay_init(void);
int relay_gpio(int n);
esp_err_t relay_set(int n, bool on);
bool relay_get(int n);
