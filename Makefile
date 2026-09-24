SDK ?= build/sdk
SDK := $(abspath $(SDK))
BUILD ?= build/apps
DESTDIR ?= $(BUILD)/install
INSTALL_PROGRAMS := shell cat ls mkdir rm rmdir mv date mandelbrot
.DEFAULT_GOAL := all

ifneq ($(MAKECMDGOALS),clean)
ifeq ($(wildcard $(SDK)/share/pyxis.mk),)
$(error Missing SDK at $(SDK); supply SDK=/path/to/sdk exported by Pyxis)
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
                   $(BUILD)/rm/main.o $(BUILD)/rmdir/main.o \
                   $(BUILD)/mv/main.o $(BUILD)/date/main.o \
                   $(BUILD)/shell/main.o $(BUILD)/mandelbrot/main.o
SHELL_OBJECTS := $(BUILD)/shell/parse.o $(BUILD)/shell/directory.o \
                 $(BUILD)/shell/launch.o $(BUILD)/shell/command.o \
                 $(BUILD)/shell/script.o
UTILITY_OBJECT := $(BUILD)/common/directory.o

.PHONY: all install hello client server cat ls mkdir rm rmdir mv date shell mandelbrot clean FORCE
all: $(INSTALL_PROGRAMS) $(BUILD)/share/hello.txt

# Publish only the boot payload, never objects or debug ELFs. Recreate it so
# removed programs/assets cannot survive from an earlier install.
install: all
	@set -eu; \
	  staging="$(DESTDIR).tmp"; \
	  trap 'rm -rf -- "$$staging"' EXIT; \
	  rm -rf -- "$$staging"; \
	  mkdir -p "$$staging/share"; \
	  for program in $(INSTALL_PROGRAMS); do \
	    install -m 644 "$(BUILD)/$$program.pxe" "$$staging/"; \
	  done; \
	  install -m 644 init.sh "$$staging/init"; \
	  install -m 644 hello/message.txt "$$staging/share/hello.txt"; \
	  if ! diff -qr "$$staging" "$(DESTDIR)" >/dev/null 2>&1; then \
	    rm -rf -- "$(DESTDIR)"; \
	    mv -- "$$staging" "$(DESTDIR)"; \
	  fi

hello: $(BUILD)/hello.pxe $(BUILD)/share/hello.txt
client: $(BUILD)/client.pxe
server: $(BUILD)/server.pxe
cat: $(BUILD)/cat.pxe
ls: $(BUILD)/ls.pxe
mkdir: $(BUILD)/mkdir.pxe
rm: $(BUILD)/rm.pxe
rmdir: $(BUILD)/rmdir.pxe
mv: $(BUILD)/mv.pxe
date: $(BUILD)/date.pxe
shell: $(BUILD)/shell.pxe
mandelbrot: $(BUILD)/mandelbrot.pxe

$(BUILD)/shell.elf: $(SHELL_OBJECTS) $(UTILITY_OBJECT)

$(BUILD)/ls.elf $(BUILD)/mkdir.elf $(BUILD)/rm.elf $(BUILD)/rmdir.elf: $(UTILITY_OBJECT)

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
