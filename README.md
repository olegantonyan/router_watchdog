# router_watchdog

ESP32-C5 firmware (ESP-IDF 6.1) with an interactive shell on USB-Serial-JTAG.

## Build & run

```sh
make build
make flash-monitor              # TTY_PORT=/dev/ttyACM0 by default
```

Settings live in `sdkconfig.defaults`; `sdkconfig` is generated from it (`rm sdkconfig && idf.py set-target esp32c5` to regenerate).

## Shell

Built on ESP-IDF `esp_console`: line editing, history, Tab completion, getopt-style args via argtable3, `help [cmd]`.

| Command | Description |
|---|---|
| `sysinfo` | chip, revision, cores, features, flash, MAC addresses |
| `version` | app version, ESP-IDF version, build time, ELF SHA |
| `uptime` | time since boot, last reset reason |
| `reboot [-d ms]` | restart the chip |
| `free [-h]` | heap per memory region |
| `ps` | FreeRTOS tasks: state, priority, core, stack high-water mark, CPU % since boot |

## Layout

```
main/main.c              shell_init() → register command groups → shell_start()
components/shell/        REPL setup only
components/cmd_system/   sysinfo, version, uptime, reboot, free, ps
```

## Adding commands

Create `components/cmd_<feature>/` exposing `esp_err_t cmd_<feature>_register(void)`, register handlers with `esp_console_cmd_register()` (use an argtable for flags, see `cmd_sys.c` `reboot`), and call it from `main.c` before `shell_start()`.
