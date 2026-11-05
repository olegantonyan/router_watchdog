#include "driver/gpio.h"
#include "esp_check.h"
#include "sdkconfig.h"
#include "relay.h"

#ifdef CONFIG_RW_RELAY_ACTIVE_LOW
#define LEVEL(on) (!(on))
#else
#define LEVEL(on) (on)
#endif

static const char *TAG = "relay";

static const int s_gpio[RELAY_COUNT] = {
    CONFIG_RW_RELAY1_GPIO, CONFIG_RW_RELAY2_GPIO, CONFIG_RW_RELAY3_GPIO, CONFIG_RW_RELAY4_GPIO,
    CONFIG_RW_RELAY5_GPIO, CONFIG_RW_RELAY6_GPIO, CONFIG_RW_RELAY7_GPIO, CONFIG_RW_RELAY8_GPIO,
};
static bool s_on[RELAY_COUNT];

esp_err_t relay_init(void)
{
    for (int i = 0; i < RELAY_COUNT; i++) {
        int gpio = s_gpio[i];
        if (gpio < 0) {
            continue;
        }
        ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(gpio), ESP_ERR_INVALID_ARG, TAG, "relay %d: GPIO%d can't output", i + 1, gpio);
        gpio_set_level(gpio, LEVEL(false));
        gpio_config_t cfg = { .pin_bit_mask = 1ULL << gpio, .mode = GPIO_MODE_OUTPUT };
        ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "relay %d: GPIO%d", i + 1, gpio);
    }
    return ESP_OK;
}

int relay_gpio(int n)
{
    return n >= 1 && n <= RELAY_COUNT ? s_gpio[n - 1] : -1;
}

esp_err_t relay_set(int n, bool on)
{
    int gpio = relay_gpio(n);
    ESP_RETURN_ON_FALSE(gpio >= 0, ESP_ERR_INVALID_ARG, TAG, "relay %d: not connected", n);
    ESP_RETURN_ON_ERROR(gpio_set_level(gpio, LEVEL(on)), TAG, "relay %d", n);
    s_on[n - 1] = on;
    return ESP_OK;
}

bool relay_get(int n)
{
    return relay_gpio(n) >= 0 && s_on[n - 1];
}
