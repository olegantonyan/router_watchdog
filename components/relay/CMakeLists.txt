idf_component_register(SRCS "relay.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_driver_gpio)
