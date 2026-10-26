idf_component_register(SRCS "clock.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_event esp_netif lwip)
