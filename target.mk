# Installed as SDK/share/pyxis.mk. Resolve paths from this file so the SDK moves
# as one directory; the compiler remains an external, prebuilt dependency.
PYXIS_SDK := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
PYXIS_SYSROOT := $(PYXIS_SDK)/sysroot
CROSS_COMPILE ?= x86_64-unknown-pyxis-
# The SDK records the toolchain (gcc or llvm) that built its runtime archives;
# its consumers use the same one.
include $(PYXIS_SDK)/share/toolchain.mk
ifeq ($(PYXIS_TOOLCHAIN),llvm)
CC := $(CROSS_COMPILE)clang
AR := $(shell $(CC) -print-prog-name=llvm-ar)
PYXIS_COMPILER_ID := $(shell $(CC) -dumpmachine) $(shell $(CC) -dumpversion)
PYXIS_RUNTIME := $(shell $(CC) -print-libgcc-file-name)
PYXIS_RUNTIME_LIBRARY := clang_rt.builtins
else
CC := $(CROSS_COMPILE)gcc
AR := $(CROSS_COMPILE)ar
PYXIS_COMPILER_ID := $(shell $(CC) -dumpmachine) $(shell $(CC) -dumpfullversion)
PYXIS_RUNTIME := -lgcc
PYXIS_RUNTIME_LIBRARY := gcc
endif
# PYXIS_RUNTIME_LIBRARY is the -l name of the runtime archive the SDK sysroot
# exports, for tools that link it by name, such as TCC.

# Keep SDK headers on a normal include path so -MMD tracks their changes.
# Only compiler-provided headers and exported target headers are visible.
# SDK -I directories take precedence over the compiler's -isystem directory.
PYXIS_CPPFLAGS := --sysroot=$(PYXIS_SYSROOT) -nostdinc \
                  -isystem $(shell $(CC) -print-file-name=include) \
                  -I$(PYXIS_SYSROOT)/usr/include
PYXIS_CFLAGS := -std=gnu23 -O2 -g3 -ffreestanding -fno-stack-protector \
               -fno-pic -fno-pie -mno-red-zone -march=x86-64 \
               -Wall -Wextra -MMD -MP
PYXIS_LINKER_SCRIPT := $(PYXIS_SYSROOT)/usr/lib/pyxis.ld
PYXIS_LDFLAGS := --sysroot=$(PYXIS_SYSROOT) -nostdlib -static -no-pie \
                -Wl,-T,$(PYXIS_LINKER_SCRIPT) \
                -Wl,--build-id=none -Wl,-z,max-page-size=0x1000
# The Pyxis Clang driver links P1F executables. Consumers still link an ELF
# and convert it with elf2pxe while both toolchains are supported.
ifeq ($(PYXIS_TOOLCHAIN),llvm)
PYXIS_LDFLAGS += -Wl,--oformat=elf
endif
PYXIS_START := $(PYXIS_SYSROOT)/usr/lib/crt0.o
PYXIS_LIBRARIES := $(PYXIS_SYSROOT)/usr/lib/libc.a \
                   $(PYXIS_SYSROOT)/usr/lib/libterm.a $(PYXIS_SYSROOT)/usr/lib/libpyxis.a
PYXIS_LDLIBS := -Wl,--start-group $(PYXIS_LIBRARIES) $(PYXIS_RUNTIME) -Wl,--end-group
PYXIS_ELF2PXE := $(PYXIS_SDK)/bin/elf2pxe
