#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "led.h"

#define LED_TICK_MS 20
#define LED_MODE LEDC_LOW_SPEED_MODE
#define LED_CHANNEL LEDC_CHANNEL_0
#define LED_DUTY_BITS LEDC_TIMER_13_BIT
#define LED_DUTY_MAX (1u << LED_DUTY_BITS)

static const char *TAG = "led";

static const char *s_effect_names[] = {
    [LED_OFF] = "off",
    [LED_ON] = "on",
    [LED_BLINK] = "blink",
    [LED_BREATHE] = "breathe",
};

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static led_state_t s_state = { .brightness = 100 };
static led_auto_fn_t s_auto_fn;

static uint32_t render(const led_state_t *st, uint32_t now_ms)
{
    uint32_t period = st->period_ms ? st->period_ms : 1000;
    uint32_t phase = now_ms % period;
    switch (st->effect) {
    case LED_ON:
        return 255;
    case LED_BLINK:
        return phase < period / 2 ? 255 : 0;
    case LED_BREATHE: {
        uint32_t half = period / 2;
        uint32_t level = phase < half ? phase * 255 / half : (period - phase) * 255 / half;
        return level * level / 255;
    }
    default:
        return 0;
    }
}

static void led_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    uint32_t shown = UINT32_MAX;
    for (;;) {
        led_state_t st;
        taskENTER_CRITICAL(&s_lock);
        st = s_state;
        led_auto_fn_t auto_fn = s_auto_fn;
        taskEXIT_CRITICAL(&s_lock);

        if (st.auto_mode && auto_fn) {
            auto_fn(&st.effect, &st.period_ms);
            taskENTER_CRITICAL(&s_lock);
            if (s_state.auto_mode) {
                s_state.effect = st.effect;
                s_state.period_ms = st.period_ms;
            }
            taskEXIT_CRITICAL(&s_lock);
        }

        uint32_t duty = render(&st, esp_timer_get_time() / 1000) * st.brightness * LED_DUTY_MAX / (255 * 100);
        if (duty != shown) {
            ledc_set_duty(LED_MODE, LED_CHANNEL, duty);
            ledc_update_duty(LED_MODE, LED_CHANNEL);
            shown = duty;
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(LED_TICK_MS));
    }
}

esp_err_t led_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LED_MODE,
        .duty_resolution = LED_DUTY_BITS,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "timer");
    ledc_channel_config_t ch_cfg = {
        .gpio_num = CONFIG_RW_LED_GPIO,
        .speed_mode = LED_MODE,
        .channel = LED_CHANNEL,
        .timer_sel = LEDC_TIMER_0,
    };
#ifdef CONFIG_RW_LED_ACTIVE_LOW
    ch_cfg.flags.output_invert = 1;
#endif
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "channel");
    ESP_RETURN_ON_FALSE(xTaskCreate(led_task, "led", 2048, NULL, 1, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void led_set(led_effect_t effect, uint32_t period_ms)
{
    taskENTER_CRITICAL(&s_lock);
    s_state.auto_mode = false;
    s_state.effect = effect;
    s_state.period_ms = period_ms;
    taskEXIT_CRITICAL(&s_lock);
}

void led_set_auto(led_auto_fn_t fn)
{
    taskENTER_CRITICAL(&s_lock);
    s_auto_fn = fn;
    s_state.auto_mode = true;
    taskEXIT_CRITICAL(&s_lock);
}

bool led_auto(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool ok = s_auto_fn != NULL;
    if (ok) {
        s_state.auto_mode = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    return ok;
}

void led_set_brightness(uint8_t percent)
{
    taskENTER_CRITICAL(&s_lock);
    s_state.brightness = percent > 100 ? 100 : percent;
    taskEXIT_CRITICAL(&s_lock);
}

void led_get(led_state_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_state;
    taskEXIT_CRITICAL(&s_lock);
}

const char *led_effect_name(led_effect_t effect)
{
    return (size_t)effect < sizeof(s_effect_names) / sizeof(s_effect_names[0]) ? s_effect_names[effect] : "?";
}
