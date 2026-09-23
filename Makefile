CROSS_COMPILE ?= x86_64-elf-
CC := $(CROSS_COMPILE)gcc
AR := $(CROSS_COMPILE)ar
CPPFLAGS := -Ilibc/include -Iinclude -I../include
CFLAGS := -std=gnu23 -O2 -g3 -ffreestanding -fno-stack-protector \
          -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only \
          -Wall -Wextra -MMD -MP
LDFLAGS := -nostdlib -static -no-pie -Wl,-T,linker.ld \
           -Wl,--build-id=none -Wl,-z,max-page-size=0x1000

START_OBJECT := ../build/userspace/libc/start.o
LIBPYXIS := ../build/userspace/libpyxis.a
LIB_OBJECTS := ../build/userspace/lib/startup.o \
               ../build/userspace/lib/console.o ../build/userspace/lib/handle.o \
               ../build/userspace/lib/file.o ../build/userspace/lib/endpoint.o \
               ../build/userspace/lib/directory.o ../build/userspace/lib/path.o \
               ../build/userspace/lib/memory.o ../build/userspace/lib/process.o \
               ../build/userspace/lib/launcher.o ../build/userspace/lib/program.o \
               ../build/userspace/lib/shebang.o
LIBTERM := ../build/userspace/libterm.a
TERM_OBJECTS := ../build/userspace/libterm/term.o ../build/userspace/libterm/line.o
LIBC := ../build/userspace/libc.a
LIBC_SOURCES := $(wildcard libc/*.c)
LIBC_OBJECTS := $(patsubst %.c,../build/userspace/%.o,$(LIBC_SOURCES)) \
                ../build/userspace/libc/tlsf.o
PROGRAM_OBJECTS := ../build/userspace/hello/main.o ../build/userspace/client/main.o \
                   ../build/userspace/server/main.o ../build/userspace/cat/main.o \
                   ../build/userspace/ls/main.o ../build/userspace/mkdir/main.o \
                   ../build/userspace/shell/main.o
SHELL_OBJECTS := ../build/userspace/shell/parse.o ../build/userspace/shell/directory.o \
                 ../build/userspace/shell/launch.o
UTILITY_OBJECT := ../build/userspace/common/directory.o

.PHONY: all libpyxis libterm libc hello client server cat ls mkdir shell converter clean
all: shell cat ls mkdir ../build/userspace/share/hello.txt

libpyxis: $(LIBPYXIS)
libterm: $(LIBTERM)
libc: $(LIBC)
hello: ../build/userspace/hello.pxe ../build/userspace/share/hello.txt
client: ../build/userspace/client.pxe
server: ../build/userspace/server.pxe
cat: ../build/userspace/cat.pxe
ls: ../build/userspace/ls.pxe
mkdir: ../build/userspace/mkdir.pxe
shell: ../build/userspace/shell.pxe

../build/userspace/shell.elf: $(SHELL_OBJECTS) $(UTILITY_OBJECT)

../build/userspace/ls.elf ../build/userspace/mkdir.elf: $(UTILITY_OBJECT)

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

$(LIBTERM): $(TERM_OBJECTS) Makefile
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $(TERM_OBJECTS)

$(LIBC): $(LIBC_OBJECTS) Makefile
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $(LIBC_OBJECTS)

../build/userspace/lib/shebang.o: ../lib/shebang.c ../include/pxe/shebang.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

../build/userspace/libc/tlsf.o: ../third_party/tlsf/tlsf.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTLSF_USERSPACE -Ilibc -c $< -o $@

../build/userspace/libc/malloc.o: CPPFLAGS += -I../third_party/tlsf

# Startup is always linked; the archive supplies only referenced wrappers.
../build/userspace/%.elf: ../build/userspace/%/main.o $(START_OBJECT) $(LIBPYXIS) $(LIBTERM) $(LIBC) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(filter %.o,$^) -Wl,--start-group $(LIBC) $(LIBTERM) $(LIBPYXIS) -Wl,--end-group

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

-include $(START_OBJECT:.o=.d) $(LIB_OBJECTS:.o=.d) $(TERM_OBJECTS:.o=.d) $(LIBC_OBJECTS:.o=.d) $(PROGRAM_OBJECTS:.o=.d) $(UTILITY_OBJECT:.o=.d) $(SHELL_OBJECTS:.o=.d)
