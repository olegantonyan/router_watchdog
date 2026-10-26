#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/time.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "sdkconfig.h"
#include "clock.h"

#define EPOCH_2025_01_01 1735689600

_Static_assert(CLOCK_MAX_SERVERS <= CONFIG_LWIP_SNTP_MAX_SERVERS, "raise CONFIG_LWIP_SNTP_MAX_SERVERS");

static const char *TAG = "clock";

static char s_tz[64] = CONFIG_RW_TZ;
static char s_servers[CLOCK_MAX_SERVERS][64] = { CONFIG_RW_NTP_SERVER1, CONFIG_RW_NTP_SERVER2 };
static volatile uint32_t s_sync_count;
static volatile time_t s_last_sync;

static void on_sync(struct timeval *tv)
{
    s_last_sync = tv->tv_sec;
    s_sync_count++;
    char buf[32];
    struct tm tm;
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", localtime_r(&tv->tv_sec, &tm));
    ESP_LOGI(TAG, "time synced: %s", buf);
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_sntp_restart();
}

esp_err_t clock_init(void)
{
    setenv("TZ", s_tz, 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST(s_servers[0], s_servers[1]));
    cfg.wait_for_sync = false;
    cfg.sync_cb = on_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    return esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
}

bool clock_is_set(void)
{
    return time(NULL) > EPOCH_2025_01_01;
}

uint32_t clock_sync_count(void)
{
    return s_sync_count;
}

time_t clock_last_sync(void)
{
    return s_last_sync;
}

uint32_t clock_sync_interval_s(void)
{
    return esp_sntp_get_sync_interval() / 1000;
}

void clock_resync(void)
{
    esp_sntp_restart();
}

const char *clock_server(int idx)
{
    return idx >= 0 && idx < CLOCK_MAX_SERVERS && s_servers[idx][0] ? s_servers[idx] : NULL;
}

uint8_t clock_server_reach(int idx)
{
    return clock_server(idx) ? esp_sntp_getreachability(idx) : 0;
}

esp_err_t clock_set_server(int idx, const char *host)
{
    if (idx < 0 || idx >= CLOCK_MAX_SERVERS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (host && strlen(host) >= sizeof(s_servers[idx])) {
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(s_servers[idx], host ? host : "", sizeof(s_servers[idx]));
    esp_sntp_setservername(idx, host ? s_servers[idx] : NULL);
    esp_sntp_restart();
    return ESP_OK;
}

const char *clock_tz(void)
{
    return s_tz;
}

esp_err_t clock_set_tz(const char *posix)
{
    if (!posix || !(isalpha((unsigned char)posix[0]) || posix[0] == '<')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(posix) >= sizeof(s_tz)) {
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(s_tz, posix, sizeof(s_tz));
    setenv("TZ", s_tz, 1);
    tzset();
    return ESP_OK;
}
