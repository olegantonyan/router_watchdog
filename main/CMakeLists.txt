idf_component_register(SRCS "main.c" "status_led.c"
                       PRIV_REQUIRES nvs_flash clock inet led net web shell cmd_system cmd_net cmd_led cmd_clock)
