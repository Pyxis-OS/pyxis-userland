CROSS_COMPILE ?= x86_64-elf-
CC := $(CROSS_COMPILE)gcc
AR := $(CROSS_COMPILE)ar
CPPFLAGS := -Iinclude -I../include
CFLAGS := -std=gnu23 -O2 -g3 -ffreestanding -fno-stack-protector \
          -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only \
          -Wall -Wextra -MMD -MP
LDFLAGS := -nostdlib -static -no-pie -Wl,-T,linker.ld \
           -Wl,--build-id=none -Wl,-z,max-page-size=0x1000

START_OBJECT := ../build/userspace/lib/start.o
LIBPYXIS := ../build/userspace/libpyxis.a
LIB_OBJECTS := ../build/userspace/lib/startup.o ../build/userspace/lib/exit.o \
               ../build/userspace/lib/console.o ../build/userspace/lib/handle.o \
               ../build/userspace/lib/file.o ../build/userspace/lib/endpoint.o \
               ../build/userspace/lib/directory.o
PROGRAM_OBJECTS := ../build/userspace/hello/main.o ../build/userspace/client/main.o \
                   ../build/userspace/server/main.o

.PHONY: all libpyxis hello client server converter clean
all: hello client server

libpyxis: $(LIBPYXIS)
hello: ../build/userspace/hello.pxe ../build/userspace/share/hello.txt
client: ../build/userspace/client.pxe
server: ../build/userspace/server.pxe

../build/userspace/share/hello.txt: hello/message.txt
	@mkdir -p $(@D)
	cp $< $@

converter:
	$(MAKE) -C ../tools elf2pxe

# Consult the tools Makefile even when the converter binary already exists.
../build/tools/elf2pxe: converter

$(LIBPYXIS): $(LIB_OBJECTS) Makefile
	@mkdir -p $(@D)
	# Recreate the archive so removed library objects cannot remain as members.
	rm -f $@
	$(AR) rcs $@ $(LIB_OBJECTS)

# Startup is always linked; the archive supplies only referenced wrappers.
../build/userspace/%.elf: ../build/userspace/%/main.o $(START_OBJECT) $(LIBPYXIS) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(START_OBJECT) $< $(LIBPYXIS)

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

# Keep the ELF symbols and intermediate objects for debugging and rebuilds.
.SECONDARY:

-include $(START_OBJECT:.o=.d) $(LIB_OBJECTS:.o=.d) $(PROGRAM_OBJECTS:.o=.d)
