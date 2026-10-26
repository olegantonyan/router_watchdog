#include "net.h"
#include "status_led.h"

void status_led_pattern(led_effect_t *effect, led_rgb_t *color, uint32_t *period_ms)
{
    if (net_has_ip()) {
        *effect = LED_SOLID;
        *color = (led_rgb_t){ 0, 255, 0 };
        *period_ms = 0;
    } else if (net_associated() || net_reconnect_enabled()) {
        *effect = LED_BLINK;
        *color = (led_rgb_t){ 255, 180, 0 };
        *period_ms = 500;
    } else {
        *effect = LED_BREATHE;
        *color = (led_rgb_t){ 0, 0, 255 };
        *period_ms = 3000;
    }
}
