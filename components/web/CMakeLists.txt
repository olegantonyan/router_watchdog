idf_component_register(SRCS "web.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES clock net esp_http_server esp_wifi esp_netif esp_timer esp_app_format heap)
