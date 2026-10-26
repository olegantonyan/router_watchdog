#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"

esp_err_t net_init(void);

esp_netif_t *net_sta(void);
esp_err_t net_connect(const char *ssid, const char *password);
void net_disconnect(void);
bool net_associated(void);
bool net_has_ip(void);
bool net_reconnect_enabled(void);
uint8_t net_last_reason(void);

esp_netif_t *net_ap(void);
const char *net_ap_ssid(void);

const char *net_reason_str(uint8_t reason);
const char *net_auth_str(wifi_auth_mode_t mode);
const char *net_band_str(uint8_t channel);
