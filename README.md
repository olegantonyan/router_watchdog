# router_watchdog

ESP32-C5 firmware (ESP-IDF 6.1) with an interactive shell on USB-Serial-JTAG.

## Build & run

```sh
make build
make flash-monitor              # TTY_PORT=/dev/ttyACM0 by default
```

Settings live in `sdkconfig.defaults`; `sdkconfig` is generated from it (`rm sdkconfig && idf.py set-target esp32c5` to regenerate).

## Shell

Built on ESP-IDF `esp_console`: line editing, history, Tab completion, getopt-style args via argtable3, `help [cmd]`. Long-running commands stop on Ctrl+C.

| Command | Description |
|---|---|
| `sysinfo` | chip, revision, cores, features, flash, MAC addresses |
| `version` | app version, ESP-IDF version, build time, ELF SHA |
| `uptime` | time since boot, last reset reason |
| `reboot [-d ms]` | restart the chip |
| `free [-h]` | heap per memory region |
| `ps` | FreeRTOS tasks: state, priority, core, stack high-water mark, CPU % since boot |
| `wifi scan` | nearby networks: SSID, BSSID, channel, band, RSSI, auth |
| `wifi connect [-t sec] [-b 2.4\|5\|auto] <ssid> [<password>]` | join a network (DHCP); kept in RAM only, auto-reconnects until `wifi disconnect` |
| `wifi disconnect` | leave and stop reconnecting |
| `wifi status` | state, signal, IP, gateway, DNS, last error |
| `ping [-c n] [-i ms] [-s bytes] [-W ms] <host>` | ICMP echo; `-c 0` runs until Ctrl+C |
| `nc [-uCv] [-w sec] <host> <port>` | TCP/UDP client; type lines, Ctrl+C/Ctrl+D quits |
| `nc -l [-uCv] <port>` | accept one TCP connection, or receive UDP and reply to the last sender |
| `nc -z [-w sec] <host> <port>` | TCP port check: prints open/closed |

## Layout

```
main/main.c              NVS init, shell_init() → register command groups → shell_start()
components/shell/        REPL setup + helpers: shell_args_ok, shell_subcmd_run, shell_getc
components/cmd_system/   sysinfo, version, uptime, reboot, free, ps
components/cmd_net/      Wi-Fi station, wifi, ping, nc
```

## Adding commands

Create `components/cmd_<feature>/` exposing `esp_err_t cmd_<feature>_register(void)`, register handlers with `esp_console_cmd_register()`, and call it from `main.c` before `shell_start()`.

- Flags: an argtable struct + `shell_args_ok()` (see `reboot` in `cmd_sys.c`).
- Subcommands (`foo bar ...`): a `shell_subcmd_t` table + `shell_subcmd_run()` (see `cmd_wifi.c`).
- Long-running loops: poll `shell_getc()` and stop on `SHELL_CTRL_C`.
