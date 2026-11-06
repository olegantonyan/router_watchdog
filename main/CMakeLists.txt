idf_component_register(SRCS "main.c" "status_led.c"
                       PRIV_REQUIRES nvs_flash clock inet led relay watchdog net web telnet shell cmd_system cmd_net cmd_led cmd_relay cmd_watchdog cmd_clock)
