idf_component_register(SRCS "cmd_clock.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES clock shell console)
