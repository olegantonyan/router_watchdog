idf_component_register(SRCS "cmd_net.c" "cmd_wifi.c" "cmd_ap.c" "cmd_ping.c" "cmd_nc.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES net shell console esp_wifi esp_netif esp_timer lwip)
