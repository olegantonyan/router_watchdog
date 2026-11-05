#include "inet.h"
#include "net.h"
#include "status_led.h"

void status_led_pattern(led_effect_t *effect, uint32_t *period_ms)
{
    inet_state_t inet = inet_state();
    *period_ms = 0;
    if (net_has_ip() && inet == INET_ONLINE) {
        *effect = LED_ON;
    } else if (net_has_ip() && inet == INET_OFFLINE) {
        *effect = LED_BLINK;
        *period_ms = 250;
    } else if (net_has_ip() || net_associated() || net_reconnect_enabled()) {
        *effect = LED_BLINK;
        *period_ms = 1000;
    } else {
        *effect = LED_BREATHE;
        *period_ms = 3000;
    }
}
