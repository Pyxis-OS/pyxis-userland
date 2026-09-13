CROSS_COMPILE ?= x86_64-elf-
CC := $(CROSS_COMPILE)gcc
CFLAGS := -std=gnu23 -O2 -g3 -ffreestanding -fno-stack-protector \
          -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only \
          -Wall -Wextra -MMD -MP
LDFLAGS := -nostdlib -static -no-pie -Wl,-T,linker.ld \
           -Wl,--build-id=none -Wl,-z,max-page-size=0x1000

.PHONY: all hello converter clean
all: hello

hello: ../build/userspace/hello.pxe

converter:
	$(MAKE) -C ../tools elf2pxe

# Consult the tools Makefile even when the converter binary already exists.
../build/tools/elf2pxe: converter

../build/userspace/hello.elf: ../build/userspace/hello/start.o linker.ld
	$(CC) $(LDFLAGS) -o $@ $<

../build/userspace/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

../build/userspace/%.pxe: ../build/userspace/%.elf ../build/tools/elf2pxe
	../build/tools/elf2pxe --format p1f -o $@ $<

clean:
	rm -rf ../build/userspace

-include ../build/userspace/hello/start.d
