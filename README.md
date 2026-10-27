# router_watchdog

ESP32-C5 firmware (ESP-IDF 6.1) with an interactive shell on USB-Serial-JTAG.

## Build & run

```sh
make build
make flash-monitor              # TTY_PORT=/dev/ttyACM0 by default
```

Settings live in `sdkconfig.defaults`; `sdkconfig` is generated from it (`rm sdkconfig && idf.py set-target esp32c5` to regenerate).

## Access point & web

On boot the board starts an access point, alongside the Wi-Fi client:

| Setting | Default |
|---|---|
| SSID | `router-watchdog-XXXX` (last 2 bytes of the AP MAC) |
| Password | `watchdog` (WPA2) |
| IP | `192.168.4.1/24`, DHCP for clients |
| Channel | 1, then follows the client's channel once `wifi connect` succeeds (single radio) |

Change them in `idf.py menuconfig` → Router watchdog.

Web server (no auth, port 80, reachable on the AP and on the client network):
- `http://192.168.4.1/` — status page, refreshes every 5 s
- `http://192.168.4.1/api/status` — same data as JSON

## Wi-Fi client

A `wifi connect` that gets an IP saves SSID, password and band to NVS (namespace `wifi`, unencrypted); the board joins that network on every boot. `wifi disconnect` keeps it saved, `wifi forget` erases it. Reconnects back off after 3 quick retries: 5, 10, 20, 40, then every 60 s.

## RGB LED

The DevKitC-1 addressable LED (GPIO27, `idf.py menuconfig` → Router watchdog LED) shows network status by default:

| State | LED |
|---|---|
| Wi-Fi client connected | green, solid |
| connecting / waiting for IP | yellow, blinking |
| client idle (access point only) | blue, breathing |

`led color|blink|breathe|rainbow|off` switch to manual mode; `led auto` returns to status mode. Settings are RAM-only.

## Clock

System time is kept in software by ESP-IDF (RTC timer), survives every reset except power loss, and is synced over NTP once the Wi-Fi client has an IP, then hourly.

| Setting | Default (`idf.py menuconfig` → Router watchdog clock) |
|---|---|
| Timezone | `CET-1CEST,M3.5.0,M10.5.0/3` (Central Europe, auto DST) |
| NTP servers | `pool.ntp.org`, `time.google.com` (slot 3 free) |

`tz` and `ntp server` changes are RAM-only.

## Shell

Built on ESP-IDF `esp_console`: line editing, history, Tab completion, getopt-style args via argtable3, `help [cmd]`. Long-running commands stop on Ctrl+C.

| Command | Description |
|---|---|
| `sysinfo` | chip, revision, cores, features, flash, MAC addresses |
| `version` | app version, ESP-IDF version, build time, ELF SHA |
| `uptime` | local time (once set), time since boot, last reset reason |
| `date [-u] [+format]` | local time, UTC, or strftime format (`date +%Y-%m-%dT%H:%M:%S%z`) |
| `date -s "YYYY-MM-DD HH:MM[:SS]"` | set local time manually |
| `tz [-l] [<zone>]` | show timezone, list named zones, or set by name (`Europe/London`) or POSIX string |
| `ntp status` | sync count, last sync, interval, servers and reachability |
| `ntp sync [-t sec]` | sync now and wait |
| `ntp server <1-3> <host\|none>` | replace or clear a server |
| `reboot [-d ms]` | restart the chip |
| `free [-h]` | heap per memory region |
| `ps` | FreeRTOS tasks: state, priority, core, stack high-water mark, CPU % since boot |
| `wifi scan` | nearby networks: SSID, BSSID, channel, band, RSSI, auth |
| `wifi connect [-t sec] [-b 2.4\|5\|auto] [-n] [<ssid> [<password>]]` | join a network (DHCP), saved once it gets an IP (`-n`: don't save); no ssid joins the saved one; auto-reconnects until `wifi disconnect` |
| `wifi disconnect` | leave and stop reconnecting; saved network is kept |
| `wifi status` | state, signal, IP, gateway, DNS, last error, saved network |
| `wifi saved [-p]` | saved network: ssid, password (masked, `-p` shows it), band |
| `wifi forget` | erase the saved network; current connection stays up |
| `ap status` | AP SSID, password, auth, channel, clients, IP, DHCP range, web URL |
| `ap clients` | connected clients: MAC, IP, RSSI |
| `led status` | mode, effect, color, period, brightness |
| `led color <color>` | solid color; `<color>` = preset name, `#rrggbb` or `r,g,b` |
| `led blink\|breathe <color> [-p ms]` | blink (default 500 ms) or fade (default 3000 ms) |
| `led rainbow [-p ms]` | hue cycle, default 5000 ms |
| `led off` / `led auto` | turn off / back to network status |
| `led brightness <0-100>` | global brightness, default 20 |
| `led presets` | list color names and effects |
| `ping [-c n] [-i ms] [-s bytes] [-W ms] <host>` | ICMP echo; `-c 0` runs until Ctrl+C |
| `nc [-uCv] [-w sec] <host> <port>` | TCP/UDP client; type lines, Ctrl+C/Ctrl+D quits |
| `nc -l [-uCv] <port>` | accept one TCP connection, or receive UDP and reply to the last sender |
| `nc -z [-w sec] <host> <port>` | TCP port check: prints open/closed |

## Layout

```
main/main.c              NVS → led_init → net_init (joins saved network) → clock_init → shell_init → register command groups → web_start → shell_start
components/clock/        timezone + SNTP (esp_netif_sntp), sync tracking
components/cmd_clock/    date, tz, ntp
main/status_led.c        network state → LED pattern (auto mode)
components/led/          RGB LED driver (espressif/led_strip over RMT), effects, color presets
components/cmd_led/      led
components/net/          Wi-Fi core: client + access point, saved network (NVS), reconnect backoff, Kconfig defaults
components/web/          HTTP server: / and /api/status
components/shell/        REPL setup + helpers: shell_args_ok, shell_subcmd_run, shell_getc
components/cmd_system/   sysinfo, version, uptime, reboot, free, ps
components/cmd_net/      wifi, ap, ping, nc
```

## Adding commands

Create `components/cmd_<feature>/` exposing `esp_err_t cmd_<feature>_register(void)`, register handlers with `esp_console_cmd_register()`, and call it from `main.c` before `shell_start()`.

- Flags: an argtable struct + `shell_args_ok()` (see `reboot` in `cmd_sys.c`).
- Subcommands (`foo bar ...`): a `shell_subcmd_t` table + `shell_subcmd_run()` (see `cmd_wifi.c`).
- Long-running loops: poll `shell_getc()` and stop on `SHELL_CTRL_C`.
