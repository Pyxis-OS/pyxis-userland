# Installed as SDK/share/pyxis.mk. Resolve paths from this file so the SDK moves
# as one directory; the compiler remains an external, prebuilt dependency.
PYXIS_SDK := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
PYXIS_SYSROOT := $(PYXIS_SDK)/sysroot
CROSS_COMPILE ?= x86_64-unknown-pyxis-
CC := $(CROSS_COMPILE)clang
CXX := $(CROSS_COMPILE)clang++
AR := $(shell $(CC) -print-prog-name=llvm-ar)
PYXIS_COMPILER_ID := $(shell $(CC) -dumpmachine) $(shell $(CC) -dumpversion)
PYXIS_RUNTIME := $(shell $(CC) -print-libgcc-file-name)
PYXIS_RUNTIME_LIBRARY := clang_rt.builtins
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
# The Pyxis Clang driver links P1F executables: link straight to the .pxe file.
PYXIS_START := $(PYXIS_SYSROOT)/usr/lib/crt0.o
PYXIS_LIBRARIES := $(PYXIS_SYSROOT)/usr/lib/libc.a \
                   $(PYXIS_SYSROOT)/usr/lib/libterm.a $(PYXIS_SYSROOT)/usr/lib/libpyxis.a
PYXIS_LDLIBS := -Wl,--start-group $(PYXIS_LIBRARIES) $(PYXIS_RUNTIME) -Wl,--end-group

# C++ uses the SDK's libc++, libc++abi and libunwind. libc++'s wrappers, such
# as <stdlib.h>, include the libc headers they extend, so its directory comes
# first. No -ffreestanding: libc++ provides its hosted library.
PYXIS_CXX_CPPFLAGS := --sysroot=$(PYXIS_SYSROOT) -nostdinc \
                      -I$(PYXIS_SYSROOT)/usr/include/c++/v1 \
                      -isystem $(shell $(CC) -print-file-name=include) \
                      -I$(PYXIS_SYSROOT)/usr/include
PYXIS_CXXFLAGS := -std=gnu++23 -O2 -g3 -fno-stack-protector \
                 -fno-pic -fno-pie -mno-red-zone -march=x86-64 \
                 -Wall -Wextra -MMD -MP
PYXIS_CXX_LIBRARIES := $(PYXIS_SYSROOT)/usr/lib/libc++.a \
                       $(PYXIS_SYSROOT)/usr/lib/libc++abi.a $(PYXIS_SYSROOT)/usr/lib/libunwind.a
PYXIS_CXX_LDLIBS := -Wl,--start-group $(PYXIS_CXX_LIBRARIES) $(PYXIS_LIBRARIES) \
                    $(PYXIS_RUNTIME) -Wl,--end-group
