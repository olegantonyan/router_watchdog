idf_component_register(SRCS "cmd_watchdog.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES watchdog relay shell console esp_timer)
