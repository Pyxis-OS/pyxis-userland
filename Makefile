SDK ?= ../build/sdk
SDK := $(abspath $(SDK))
BUILD ?= ../build/userspace
.DEFAULT_GOAL := all

ifneq ($(MAKECMDGOALS),clean)
ifeq ($(wildcard $(SDK)/share/pyxis.mk),)
$(error Missing SDK at $(SDK); run make sdk in Pyxis or supply SDK=/path/to/sdk)
endif
include $(SDK)/share/pyxis.mk
endif
CPPFLAGS := $(PYXIS_CPPFLAGS)
CFLAGS := $(PYXIS_CFLAGS)
LDFLAGS := $(PYXIS_LDFLAGS)
LDLIBS := $(PYXIS_LDLIBS)
export SDK CC CPPFLAGS CFLAGS LDFLAGS LDLIBS PYXIS_COMPILER_ID

PROGRAM_OBJECTS := $(BUILD)/hello/main.o $(BUILD)/client/main.o \
                   $(BUILD)/server/main.o $(BUILD)/cat/main.o \
                   $(BUILD)/ls/main.o $(BUILD)/mkdir/main.o \
                   $(BUILD)/shell/main.o
SHELL_OBJECTS := $(BUILD)/shell/parse.o $(BUILD)/shell/directory.o \
                 $(BUILD)/shell/launch.o $(BUILD)/shell/command.o \
                 $(BUILD)/shell/script.o
UTILITY_OBJECT := $(BUILD)/common/directory.o

.PHONY: all hello client server cat ls mkdir shell clean FORCE
all: shell cat ls mkdir $(BUILD)/share/hello.txt

hello: $(BUILD)/hello.pxe $(BUILD)/share/hello.txt
client: $(BUILD)/client.pxe
server: $(BUILD)/server.pxe
cat: $(BUILD)/cat.pxe
ls: $(BUILD)/ls.pxe
mkdir: $(BUILD)/mkdir.pxe
shell: $(BUILD)/shell.pxe

$(BUILD)/shell.elf: $(SHELL_OBJECTS) $(UTILITY_OBJECT)

$(BUILD)/ls.elf $(BUILD)/mkdir.elf: $(UTILITY_OBJECT)

$(BUILD)/share/hello.txt: hello/message.txt
	@mkdir -p $(@D)
	cp $< $@

# Only exported SDK objects/libraries participate in application links.
$(BUILD)/%.elf: $(BUILD)/%/main.o $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(SDK)/share/pyxis.mk $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(filter %.o,$^) $(LDLIBS)

$(BUILD)/%.o: %.c Makefile $(SDK)/share/pyxis.mk $(BUILD)/.config
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.pxe: $(BUILD)/%.elf $(PYXIS_ELF2PXE) Makefile
	$(PYXIS_ELF2PXE) --format p1f -o $@ $<

# SDK selection and compiler flags are build inputs even if files are older.
$(BUILD)/.config: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' "$$SDK" "$$CC" "$$PYXIS_COMPILER_ID" "$$CPPFLAGS" "$$CFLAGS" "$$LDFLAGS" "$$LDLIBS" > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

FORCE:

clean:
	rm -rf $(BUILD)

.SECONDARY:

-include $(PROGRAM_OBJECTS:.o=.d) $(UTILITY_OBJECT:.o=.d) $(SHELL_OBJECTS:.o=.d)
