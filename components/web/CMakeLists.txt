idf_component_register(SRCS "web.c" "web_shell.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES clock inet net ota relay watchdog shell esp_http_server esp_wifi esp_netif esp_timer esp_app_format heap lwip freertos)
