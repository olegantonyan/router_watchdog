idf_component_register(SRCS "inet.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES net esp_http_client esp_netif esp_timer lwip)
