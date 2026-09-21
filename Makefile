CROSS_COMPILE ?= x86_64-elf-
CC := $(CROSS_COMPILE)gcc
CPPFLAGS := -Iinclude -I../include
CFLAGS := -std=gnu23 -O2 -g3 -ffreestanding -fno-stack-protector \
          -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only \
          -Wall -Wextra -MMD -MP
LDFLAGS := -nostdlib -static -no-pie -Wl,-T,linker.ld \
           -Wl,--build-id=none -Wl,-z,max-page-size=0x1000

HELLO_OBJECTS := ../build/userspace/hello/start.o ../build/userspace/hello/main.o \
                 ../build/userspace/lib/io.o ../build/userspace/lib/exit.o \
                 ../build/userspace/lib/console.o ../build/userspace/lib/handle.o \
                 ../build/userspace/lib/blob.o

.PHONY: all hello converter clean
all: hello

hello: ../build/userspace/hello.pxe ../build/userspace/hello.txt

../build/userspace/hello.txt: hello/message.txt
	@mkdir -p $(@D)
	cp $< $@

converter:
	$(MAKE) -C ../tools elf2pxe

# Consult the tools Makefile even when the converter binary already exists.
../build/tools/elf2pxe: converter

../build/userspace/hello.elf: $(HELLO_OBJECTS) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(HELLO_OBJECTS)

../build/userspace/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

../build/userspace/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

../build/userspace/%.pxe: ../build/userspace/%.elf ../build/tools/elf2pxe
	../build/tools/elf2pxe --format p1f -o $@ $<

clean:
	rm -rf ../build/userspace

-include $(HELLO_OBJECTS:.o=.d)
