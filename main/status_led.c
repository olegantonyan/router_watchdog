#include "inet.h"
#include "net.h"
#include "status_led.h"

void status_led_pattern(led_effect_t *effect, led_rgb_t *color, uint32_t *period_ms)
{
    static const led_rgb_t green = { 0, 255, 0 }, red = { 255, 0, 0 }, orange = { 255, 80, 0 };
    inet_state_t inet = inet_state();
    *period_ms = 0;
    if (net_has_ip() && inet == INET_ONLINE) {
        *effect = LED_SOLID;
        *color = green;
    } else if (net_has_ip() && inet == INET_OFFLINE) {
        *effect = LED_SOLID;
        *color = red;
    } else if (net_has_ip() || net_associated() || net_reconnect_enabled()) {
        *effect = LED_BLINK;
        *color = orange;
        *period_ms = 500;
    } else {
        *effect = LED_SOLID;
        *color = orange;
    }
}
