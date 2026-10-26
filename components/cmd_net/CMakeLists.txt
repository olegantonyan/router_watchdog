idf_component_register(SRCS "cmd_net.c" "net.c" "cmd_wifi.c" "cmd_ping.c" "cmd_nc.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES shell console esp_wifi esp_netif esp_event esp_timer lwip)
