IDF_ACTIVATE_PATH ?= $(HOME)/.espressif/tools/activate_idf_v6.1.sh
TTY_PORT ?= /dev/ttyACM0
HOST ?= 192.168.4.1
IDF := . $(IDF_ACTIVATE_PATH) >/dev/null && idf.py

.PHONY: build flash monitor flash-monitor ota menuconfig clean fullclean

build:
	$(IDF) build

flash:
	$(IDF) -p $(TTY_PORT) flash

monitor:
	$(IDF) -p $(TTY_PORT) monitor

flash-monitor:
	$(IDF) -p $(TTY_PORT) flash monitor

ota:
	curl -fsS -H 'Content-Type: application/octet-stream' --data-binary @build/router_watchdog.bin http://$(HOST)/api/update && echo

menuconfig:
	$(IDF) menuconfig

clean:
	$(IDF) clean

fullclean:
	$(IDF) fullclean
