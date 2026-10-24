# Linux host build; the parent userland Makefile builds the native Pyxis bundle.
CC ?= cc
BUILD ?= build
CFLAGS ?= -O2 -g

.PHONY: all
all: $(BUILD)/cpuclock

$(BUILD)/cpuclock: main.c Makefile
	@mkdir -p "$(@D)"
	$(CC) -std=gnu11 $(CFLAGS) -Wall -Wextra -Werror -pthread main.c -o "$@"
