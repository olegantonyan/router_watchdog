# router_watchdog

ESP32 firmware (ESP-IDF 6.1) for the ESP32-32E N4 relay board, with an interactive shell on UART0 (115200 8N1) and over telnet.

## Build & run

```sh
make build
make flash-monitor              # TTY_PORT=/dev/ttyACM0 by default
make ota HOST=192.168.4.1        # over Wi-Fi, see Firmware update
```

Settings live in `sdkconfig.defaults`; `sdkconfig` is generated from it (`rm sdkconfig && idf.py set-target esp32` to regenerate).

### Flashing over USB-UART

Use a 3.3 V logic USB-UART adapter (a 5 V TXD can damage GPIO3).

```
USB-UART adapter        Board header
GND  ─────────────────  GND
TXD  ─────────────────  RX   (GPIO3)
RXD  ─────────────────  TX   (GPIO1)
```

Power the board from its own supply and share only GND; adapter 3V3 pins can't supply Wi-Fi current peaks.

No auto-reset (EN and IO0 aren't wired to the adapter), so enter download mode with the on-board buttons:
1. Hold IO0, press and release EN, release IO0.
2. `make flash`.
3. Press EN to run the new firmware, then `make monitor`.

## Access point & web

On boot the board starts an access point, alongside the Wi-Fi client:

| Setting | Default |
|---|---|
| SSID | `router-watchdog-XXXX` (last 2 bytes of the AP MAC) |
| Password | `watchdog` (WPA2) |
| IP | `192.168.4.1/24`, DHCP for clients |
| Channel | 1, then follows the client's channel once `wifi connect` succeeds (single radio, 2.4 GHz only) |

Change them in `idf.py menuconfig` → Router watchdog.

Web server (no auth, port 80, reachable on the AP and on the client network):
- `http://192.168.4.1/` — status page (device, Wi-Fi client, internet, access point, relays, watchdog), refreshes every 5 s
- `http://192.168.4.1/api/status` — same data as JSON
- `http://192.168.4.1/shell` — shell in the browser (see Telnet)
- `http://192.168.4.1/update` — firmware upload (see Firmware update)

## Firmware update (OTA)

Two 1.7 MB app slots (`ota_0`, `ota_1`); an update writes the inactive one and reboots into it. Boards still on the single-app partition table need one serial `make flash` first (new partition table and bootloader; the saved Wi-Fi network is kept).

| Way | How |
|---|---|
| browser | `http://192.168.4.1/update`, choose `build/router_watchdog.bin`, Upload |
| curl | `make ota HOST=192.168.4.1` (POSTs the .bin to `/api/update`) |
| shell | `ota update http://<pc-ip>:8000/router_watchdog.bin` after `python3 -m http.server -d build 8000` on the PC; https works too |

Images for another project or chip are rejected before anything is written. New firmware is marked valid after running 60 s; a crash or reset before that boots the previous firmware again. `ota rollback` switches back manually. No auth: anyone on the AP or client network can flash firmware.

## Wi-Fi client

A `wifi connect` that gets an IP saves SSID and password to NVS (namespace `wifi`, unencrypted); the board joins that network on every boot. `wifi disconnect` keeps it saved, `wifi forget` erases it. Reconnects back off after 3 quick retries: 5, 10, 20, 40, then every 60 s.

## Internet check

While the Wi-Fi client has an IP, a background task runs a round of probes right away and then every 30 s:

| Probe | Pass |
|---|---|
| ping gateway, 1.1.1.1, 8.8.8.8 | any of 3 replies |
| `http://connectivitycheck.gstatic.com/generate_204`, `http://cp.cloudflare.com/generate_204` | status 204 |
| `http://www.msftconnecttest.com/connecttest.txt` | 200, `Microsoft Connect Test` |
| `http://captive.apple.com/hotspot-detect.html` | 200, `Success` |

Online = any HTTP probe passes (redirects fail, so a captive portal can't fake it); pings are diagnostics for the offline reason (`gateway unreachable`, `public IPs unreachable`, `dns failing`, `http checks failing`). Probes run in parallel, so a round takes as long as its slowest probe (~20 s worst case, when DNS times out). A total outage (no public ping, no HTTP) turns offline after one round; a partial one (pings pass, HTTP fails) needs 2 failed rounds in a row. Failed rounds repeat every 10 s. Interval and threshold: `idf.py menuconfig` → Router watchdog internet check.

## Power cycle watchdog

The core function: relay 1 cuts the router's power for 3 s (on, then back off) when either:

| Trigger | After |
|---|---|
| internet check reports offline | 30 s continuously |
| saved network has no IP (router hung or Wi-Fi gone) | 5 min continuously; not counted after `wifi disconnect` or with nothing saved |

After a cycle no new one happens for 5 min so the router can boot. Each cycle that doesn't bring the internet back doubles that wait (5, 10, 20, 40, 60 min); it resets to 5 min once the internet is online. Relay, off time, triggers and waits: `idf.py menuconfig` → Router watchdog power cycle. `watchdog disable` is RAM-only; history resets on reboot.

## Hardware

ESP32-32E N4 relay board (ESP32-WROOM-32E, 4 MB flash: NVS, two 1.7 MB OTA app slots).

| Function | GPIO |
|---|---|
| status LED D20 | 23 |
| UART0 TX / RX | 1 / 3 |
| IO0 button (BOOT) | 0 |
| EN button | chip reset |

| Relay | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| GPIO | 32 | 33 | 25 | 26 | 27 | 14 | 12 | 13 |

Relay 1 powers the router (see Power cycle watchdog).

## Status LED

LED D20 (GPIO23, PWM; pin and polarity in `idf.py menuconfig` → Router watchdog LED) shows network status by default:

| State | LED |
|---|---|
| internet online | on |
| Wi-Fi connected, internet offline | fast blink (250 ms) |
| connecting / waiting for IP / first internet check | slow blink (1 s) |
| no Wi-Fi (nothing saved or `wifi disconnect`) | breathing (3 s) |

`led on|off|blink|breathe` switch to manual mode; `led auto` returns to status mode. Settings are RAM-only.

## Relays

8 relays, GPIOs per the Hardware table; change them in `idf.py menuconfig` → Router watchdog relays (`-1` = not connected), plus an active-low option for all. Connected relays start off at boot; state is RAM-only.

## Clock

System time is kept in software by ESP-IDF (RTC timer), survives every reset except power loss, and is synced over NTP once the Wi-Fi client has an IP, then hourly.

| Setting | Default (`idf.py menuconfig` → Router watchdog clock) |
|---|---|
| Timezone | `CET-1CEST,M3.5.0,M10.5.0/3` (Central Europe, auto DST) |
| NTP servers | `pool.ntp.org`, `time.google.com` (slot 3 free) |

`tz` and `ntp server` changes are RAM-only.

## Shell

Built on ESP-IDF `esp_console`: line editing, history, Tab completion, getopt-style args via argtable3, `help [cmd]`. Long-running commands stop on Ctrl+C. Commands run one at a time: while one runs on UART or telnet, the other session waits.

### Telnet

```sh
telnet 192.168.4.1              # or the board's IP on the client network
```

Password `watchdog`, one session at a time (a second client gets `busy`), 3 wrong passwords disconnect, closes after 10 min idle. Keys: ←/→, Home/End, Backspace/Delete, ↑/↓ history (16, per session), Tab completes command names, Ctrl+A/E/U/K/W/L, Ctrl+C drops the line, `exit` or Ctrl+D on an empty line quits. Logs stay on UART except those from commands run in the telnet session. Traffic, password included, is plaintext: use it on trusted networks only. Port, password (empty = none), idle timeout or disabling it: `idf.py menuconfig` → Router watchdog telnet.

`http://192.168.4.1/shell` is the same session in the browser: a built-in terminal (no external scripts, works on the AP without internet) whose WebSocket `/api/shell` is bridged to the telnet server over loopback. Same password, keys, idle timeout and one-session limit as telnet; also plaintext. Ctrl+C interrupts unless text is selected (then it copies); Ctrl+V pastes.

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
| `wifi connect [-t sec] [-n] [<ssid> [<password>]]` | join a network (DHCP), saved once it gets an IP (`-n`: don't save); no ssid joins the saved one; auto-reconnects until `wifi disconnect` |
| `wifi disconnect` | leave and stop reconnecting; saved network is kept |
| `wifi status` | state, signal, IP, gateway, DNS, last error, saved network |
| `wifi saved [-p]` | saved network: ssid, password (masked, `-p` shows it) |
| `wifi forget` | erase the saved network; current connection stays up |
| `ap status` | AP SSID, password, auth, channel, clients, IP, DHCP range, web URL |
| `ap clients` | connected clients: MAC, IP, RSSI |
| `led status` | mode, effect, period, brightness |
| `led on` / `led off` | turn on / off |
| `led blink\|breathe [-p ms]` | blink (default 500 ms) or fade (default 3000 ms) |
| `led auto` | back to network status |
| `led brightness <0-100>` | PWM brightness, default 100 |
| `relay status` | GPIO and on/off state of relays 1-8 |
| `relay on\|off <1-8>` | switch a relay |
| `inet status` | internet state, offline reason, last/next check, per-probe result and time |
| `inet check [-t sec]` | run all probes now and wait for the result |
| `watchdog status` | state, triggers, pending condition and progress, cycle count, last cycle and reason, next wait |
| `watchdog cycle` | power cycle the router now (starts the post-cycle wait) |
| `watchdog enable\|disable` | turn automatic power cycles on/off (manual `cycle` always works) |
| `ota status` | running firmware and slot, valid / pending verify, other slot's version, update progress |
| `ota update [-n] <url>` | download an http(s) .bin, install, reboot (`-n`: don't); Ctrl+C aborts |
| `ota rollback` | reboot into the previous firmware |
| `ping [-c n] [-i ms] [-s bytes] [-W ms] <host>` | ICMP echo; `-c 0` runs until Ctrl+C |
| `nc [-uCv] [-w sec] <host> <port>` | TCP/UDP client; type lines, Ctrl+C/Ctrl+D quits |
| `nc -l [-uCv] <port>` | accept one TCP connection, or receive UDP and reply to the last sender |
| `nc -z [-w sec] <host> <port>` | TCP port check: prints open/closed |

## Layout

```
main/main.c              NVS → ota_init → relay_init → led_init → net_init (joins saved network) → clock_init → inet_start → watchdog_start → shell_init → register command groups → web_start → telnet_start → shell_start
components/clock/        timezone + SNTP (esp_netif_sntp), sync tracking
components/cmd_clock/    date, tz, ntp
main/status_led.c        network state → LED pattern (auto mode)
components/led/          status LED driver (LEDC PWM), effects
components/cmd_led/      led
components/relay/        relay GPIOs (Kconfig), on/off state
components/cmd_relay/    relay
components/watchdog/     power cycle task: offline / no-IP triggers, relay pulse, post-cycle backoff
components/cmd_watchdog/ watchdog
components/net/          Wi-Fi core: client + access point, saved network (NVS), reconnect backoff, Kconfig defaults
components/inet/         internet check task: ping + HTTP probes, online/offline state
components/ota/          OTA core: image checks, slot writes, 60 s mark-valid timer, rollback
components/cmd_ota/      ota
components/web/          HTTP server: /, /api/status, /update, /api/update, /shell + /api/shell (WebSocket → telnet bridge)
components/shell/        UART REPL (linenoise), shell_exec (one command at a time) + helpers: shell_args_ok, shell_subcmd_run, shell_getc
components/telnet/       telnet server: login, line editor, session stdin/stdout redirected to the socket
components/cmd_system/   sysinfo, version, uptime, reboot, free, ps
components/cmd_net/      wifi, ap, inet, ping, nc
```

## Adding commands

Create `components/cmd_<feature>/` exposing `esp_err_t cmd_<feature>_register(void)`, register handlers with `esp_console_cmd_register()`, and call it from `main.c` before `shell_start()`.

- Flags: an argtable struct + `shell_args_ok()` (see `reboot` in `cmd_sys.c`).
- Subcommands (`foo bar ...`): a `shell_subcmd_t` table + `shell_subcmd_run()` (see `cmd_wifi.c`).
- Long-running loops: poll `shell_getc()` and stop on `SHELL_CTRL_C`.
- Printing from another task (callbacks): use the `stdout` captured in the command handler (see `cmd_ping.c`), otherwise telnet output lands on UART.
