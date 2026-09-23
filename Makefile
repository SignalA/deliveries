CC ?= gcc
CFLAGS ?= -O2 -g
CPPFLAGS ?=
WARNINGS := -Wall -Wextra -Wpedantic -Werror
BUILD := build

.PHONY: all clean check
all: $(BUILD)/gaime_input_service $(BUILD)/game_receiver $(BUILD)/button_monitor $(BUILD)/dual_gun_acceptance

$(BUILD):
	mkdir -p $@

$(BUILD)/gaime_input_service: src/input_service.c src/evdev_input.c src/evdev_input.h src/calibration_hid.c src/calibration_hid.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=gnu11 -Isrc src/input_service.c src/evdev_input.c src/calibration_hid.c -o $@

$(BUILD)/game_receiver: examples/game_receiver.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=gnu11 examples/game_receiver.c -o $@

$(BUILD)/button_monitor: src/button_monitor.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=gnu11 src/button_monitor.c -o $@

$(BUILD)/dual_gun_acceptance: src/dual_gun_acceptance.c src/evdev_input.c src/evdev_input.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=gnu11 -Isrc src/dual_gun_acceptance.c src/evdev_input.c -o $@

check: all
	$(BUILD)/gaime_input_service --help >/dev/null
	$(BUILD)/game_receiver --help >/dev/null
	test -x $(BUILD)/button_monitor
	test -x $(BUILD)/dual_gun_acceptance

clean:
	rm -rf $(BUILD)

