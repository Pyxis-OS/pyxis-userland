SDK ?= build/sdk
SDK := $(abspath $(SDK))
BUILD ?= build/runtime
.DEFAULT_GOAL := all

ifneq ($(MAKECMDGOALS),clean)
include $(SDK)/share/pyxis.mk
endif
CPPFLAGS := -Ilibc/include -Iinclude $(PYXIS_CPPFLAGS)
CFLAGS := $(PYXIS_CFLAGS)
export SDK CC CPPFLAGS CFLAGS PYXIS_COMPILER_ID

START_OBJECT := $(BUILD)/libc/start.o
LIBPYXIS := $(BUILD)/libpyxis.a
LIB_OBJECTS := $(BUILD)/lib/startup.o \
               $(BUILD)/lib/console.o $(BUILD)/lib/handle.o \
               $(BUILD)/lib/file.o $(BUILD)/lib/endpoint.o \
               $(BUILD)/lib/directory.o $(BUILD)/lib/path.o \
               $(BUILD)/lib/memory.o $(BUILD)/lib/process.o \
               $(BUILD)/lib/launcher.o $(BUILD)/lib/program.o \
               $(BUILD)/lib/shebang.o
LIBTERM := $(BUILD)/libterm.a
TERM_OBJECTS := $(BUILD)/libterm/term.o $(BUILD)/libterm/key.o $(BUILD)/libterm/line.o
LIBC := $(BUILD)/libc.a
LIBC_SOURCES := $(wildcard libc/*.c)
LIBC_OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(LIBC_SOURCES)) \
                $(BUILD)/libc/tlsf.o

.PHONY: all libpyxis libterm libc clean FORCE
all: $(START_OBJECT) libpyxis libterm libc
libpyxis: $(LIBPYXIS)
libterm: $(LIBTERM)
libc: $(LIBC)

$(LIBPYXIS): $(LIB_OBJECTS) runtime.mk
	@mkdir -p $(@D)
	# Recreate the archive so removed library objects cannot remain as members.
	rm -f $@
	$(AR) rcs $@ $(LIB_OBJECTS)

$(LIBTERM): $(TERM_OBJECTS) runtime.mk
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $(TERM_OBJECTS)

$(LIBC): $(LIBC_OBJECTS) runtime.mk
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $(LIBC_OBJECTS)

$(BUILD)/lib/shebang.o: $(SDK)/share/pyxis/shebang.c $(SDK)/sysroot/usr/include/pxe/shebang.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/libc/tlsf.o: third_party/tlsf/tlsf.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Ilibc -c $< -o $@

$(BUILD)/libc/malloc.o: private CPPFLAGS += -Ithird_party/tlsf

$(BUILD)/%.o: %.c runtime.mk $(SDK)/share/pyxis.mk $(BUILD)/.config
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S runtime.mk $(SDK)/share/pyxis.mk $(BUILD)/.config
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(START_OBJECT) $(LIB_OBJECTS) $(TERM_OBJECTS) $(LIBC_OBJECTS): runtime.mk $(SDK)/share/pyxis.mk $(BUILD)/.config

$(BUILD)/.config: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' "$$SDK" "$$CC" "$$PYXIS_COMPILER_ID" "$$CPPFLAGS" "$$CFLAGS" > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

FORCE:

clean:
	rm -rf $(BUILD)

-include $(START_OBJECT:.o=.d) $(LIB_OBJECTS:.o=.d) $(TERM_OBJECTS:.o=.d) $(LIBC_OBJECTS:.o=.d)
