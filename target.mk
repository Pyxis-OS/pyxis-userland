# Installed as SDK/share/pyxis.mk. Resolve paths from this file so the SDK moves
# as one directory; the compiler remains an external, prebuilt dependency.
PYXIS_SDK := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
PYXIS_SYSROOT := $(PYXIS_SDK)/sysroot
CROSS_COMPILE ?= x86_64-unknown-pyxis-
CC := $(CROSS_COMPILE)gcc
AR := $(CROSS_COMPILE)ar
PYXIS_COMPILER_ID := $(shell $(CC) -dumpmachine) $(shell $(CC) -dumpfullversion)

# Keep SDK headers on a normal include path so -MMD tracks their changes.
# Only compiler-provided headers and exported target headers are visible.
PYXIS_CPPFLAGS := --sysroot=$(PYXIS_SYSROOT) -nostdinc \
                  -isystem $(shell $(CC) -print-file-name=include) \
                  -I$(PYXIS_SYSROOT)/usr/include
PYXIS_CFLAGS := -std=gnu23 -O2 -g3 -ffreestanding -fno-stack-protector \
               -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only \
               -Wall -Wextra -MMD -MP
PYXIS_LINKER_SCRIPT := $(PYXIS_SYSROOT)/usr/lib/pyxis.ld
PYXIS_LDFLAGS := --sysroot=$(PYXIS_SYSROOT) -nostdlib -static -no-pie \
                -Wl,-T,$(PYXIS_LINKER_SCRIPT) \
                -Wl,--build-id=none -Wl,-z,max-page-size=0x1000
PYXIS_START := $(PYXIS_SYSROOT)/usr/lib/crt0.o
PYXIS_LIBRARIES := $(PYXIS_SYSROOT)/usr/lib/libc.a \
                   $(PYXIS_SYSROOT)/usr/lib/libterm.a $(PYXIS_SYSROOT)/usr/lib/libpyxis.a
PYXIS_LDLIBS := -Wl,--start-group $(PYXIS_LIBRARIES) -lgcc -Wl,--end-group
PYXIS_ELF2PXE := $(PYXIS_SDK)/bin/elf2pxe
