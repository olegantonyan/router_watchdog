#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum { LED_OFF, LED_ON, LED_BLINK, LED_BREATHE } led_effect_t;

typedef struct {
    bool auto_mode;
    led_effect_t effect;
    uint32_t period_ms;
    uint8_t brightness;
} led_state_t;

typedef void (*led_auto_fn_t)(led_effect_t *effect, uint32_t *period_ms);

esp_err_t led_init(void);
void led_set(led_effect_t effect, uint32_t period_ms);
void led_set_auto(led_auto_fn_t fn);
bool led_auto(void);
void led_set_brightness(uint8_t percent);
void led_get(led_state_t *out);

const char *led_effect_name(led_effect_t effect);
