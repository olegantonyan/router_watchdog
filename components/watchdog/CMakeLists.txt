idf_component_register(SRCS "watchdog.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES inet net relay esp_timer freertos)
