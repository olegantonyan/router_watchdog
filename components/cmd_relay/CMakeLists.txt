idf_component_register(SRCS "cmd_relay.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES relay shell console)
