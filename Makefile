SDK ?= build/sdk
SDK := $(abspath $(SDK))
BUILD ?= build/apps
DESTDIR ?= $(BUILD)/install
LUA_PREFIX ?= build/ports-dev/lua
LUA_PREFIX := $(abspath $(LUA_PREFIX))
PICOHTTPPARSER_PREFIX ?= build/ports-dev/picohttpparser
PICOHTTPPARSER_PREFIX := $(abspath $(PICOHTTPPARSER_PREFIX))
HTTP_PARSER_LIBRARY := $(PICOHTTPPARSER_PREFIX)/lib/libpicohttpparser.a
LUA_LIBRARY := $(LUA_PREFIX)/lib/liblua.a
INSTALL_PROGRAMS := httpfs allocbench session shell client server counter textfs cat head ls mkdir rm rmdir mv sync date ping dig tcp ttcp udp-send udp-echo mandelbrot
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
export LUA_PREFIX PICOHTTPPARSER_PREFIX SDK CC CPPFLAGS CFLAGS LDFLAGS LDLIBS PYXIS_COMPILER_ID

PROGRAM_OBJECTS := $(BUILD)/httpfs/main.o $(BUILD)/allocbench/main.o $(BUILD)/hello/main.o $(BUILD)/client/main.o \
                   $(BUILD)/server/main.o $(BUILD)/counter/main.o $(BUILD)/textfs/main.o $(BUILD)/cat/main.o $(BUILD)/head/main.o \
                   $(BUILD)/ls/main.o $(BUILD)/mkdir/main.o \
                   $(BUILD)/rm/main.o $(BUILD)/rmdir/main.o \
                   $(BUILD)/mv/main.o $(BUILD)/sync/main.o $(BUILD)/date/main.o $(BUILD)/ping/main.o \
                   $(BUILD)/udp-send/main.o $(BUILD)/udp-echo/main.o $(BUILD)/dig/main.o $(BUILD)/tcp/main.o \
                   $(BUILD)/ttcp/main.o \
                   $(BUILD)/shell/main.o $(BUILD)/mandelbrot/main.o $(BUILD)/session/main.o
SHELL_OBJECTS := $(BUILD)/shell/parse.o $(BUILD)/shell/directory.o \
                 $(BUILD)/shell/launch.o $(BUILD)/shell/command.o \
                 $(BUILD)/shell/script.o
COUNTER_OBJECT := $(BUILD)/counter/namespace.o
SESSION_OBJECTS := $(BUILD)/session/main.o $(BUILD)/session/config.o $(BUILD)/session/network.o
HTTP_LIBRARY := $(BUILD)/libhttp.a
HTTP_OBJECTS := $(BUILD)/libhttp/uri.o $(BUILD)/libhttp/fetch.o
CONFIG_LIBRARY := $(BUILD)/libconfig.a
CONFIG_OBJECT := $(BUILD)/libconfig/config.o
DNS_LOOKUP_OBJECT := $(BUILD)/common/dns_lookup.o
DNS_OBJECTS := $(BUILD)/common/dns_message.o $(BUILD)/common/dns_query.o
UDP_OBJECT := $(BUILD)/common/udp.o
UTILITY_OBJECT := $(BUILD)/common/directory.o

.PHONY: all install httpfs allocbench session hello client server counter textfs cat head ls mkdir rm rmdir mv sync date ping dig tcp ttcp udp-send udp-echo shell mandelbrot clean FORCE
all: $(INSTALL_PROGRAMS) $(BUILD)/share/hello.txt

# Publish only the boot payload, never objects or debug ELFs. Recreate it so
# removed programs/assets cannot survive from an earlier install.
install: all
	@set -eu; \
	  staging="$(DESTDIR).tmp"; \
	  trap 'rm -rf -- "$$staging"' EXIT; \
	  rm -rf -- "$$staging"; \
	  mkdir -p "$$staging/share" "$$staging/config"; \
	  for program in $(INSTALL_PROGRAMS); do \
	    install -m 644 "$(BUILD)/$$program.pxe" "$$staging/"; \
	  done; \
	  install -m 644 init/development.sh "$$staging/init"; \
	  install -m 644 init/readonly.sh "$$staging/init-readonly"; \
	  install -m 644 init/idle.sh "$$staging/init-idle"; \
	  install -m 644 init/services.sh "$$staging/init-services"; \
	  install -m 644 config/session.lua "$$staging/config/session.lua"; \
	  install -m 644 config/network.lua "$$staging/config/network.lua"; \
	  install -m 644 hello/message.txt "$$staging/share/hello.txt"; \
	  if ! diff -qr "$$staging" "$(DESTDIR)" >/dev/null 2>&1; then \
	    rm -rf -- "$(DESTDIR)"; \
	    mv -- "$$staging" "$(DESTDIR)"; \
	  fi

allocbench: $(BUILD)/allocbench.pxe

httpfs: $(BUILD)/httpfs.pxe

session: $(BUILD)/session.pxe
hello: $(BUILD)/hello.pxe $(BUILD)/share/hello.txt
client: $(BUILD)/client.pxe
server: $(BUILD)/server.pxe
counter: $(BUILD)/counter.pxe
textfs: $(BUILD)/textfs.pxe
cat: $(BUILD)/cat.pxe
head: $(BUILD)/head.pxe
ls: $(BUILD)/ls.pxe
mkdir: $(BUILD)/mkdir.pxe
rm: $(BUILD)/rm.pxe
rmdir: $(BUILD)/rmdir.pxe
mv: $(BUILD)/mv.pxe
sync: $(BUILD)/sync.pxe
date: $(BUILD)/date.pxe
ping: $(BUILD)/ping.pxe
dig: $(BUILD)/dig.pxe
tcp: $(BUILD)/tcp.pxe
ttcp: $(BUILD)/ttcp.pxe
udp-send: $(BUILD)/udp-send.pxe
udp-echo: $(BUILD)/udp-echo.pxe
shell: $(BUILD)/shell.pxe
mandelbrot: $(BUILD)/mandelbrot.pxe

$(HTTP_OBJECTS): private CPPFLAGS += -I$(PICOHTTPPARSER_PREFIX)/include
$(HTTP_OBJECTS): $(PICOHTTPPARSER_PREFIX)/include/picohttpparser.h
$(HTTP_LIBRARY): $(HTTP_OBJECTS) $(DNS_OBJECTS) $(UDP_OBJECT) Makefile
	rm -f $@
	$(AR) rcs $@ $(HTTP_OBJECTS) $(DNS_OBJECTS) $(UDP_OBJECT)

$(BUILD)/httpfs.elf: $(BUILD)/httpfs/main.o $(HTTP_LIBRARY) $(HTTP_PARSER_LIBRARY) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(BUILD)/httpfs/main.o $(HTTP_LIBRARY) $(HTTP_PARSER_LIBRARY) $(LDLIBS)

$(SESSION_OBJECTS) $(CONFIG_OBJECT): private CPPFLAGS += -I$(LUA_PREFIX)/include
$(SESSION_OBJECTS) $(CONFIG_OBJECT): $(LUA_PREFIX)/include/lua.h $(LUA_PREFIX)/include/lauxlib.h $(LUA_PREFIX)/include/luaconf.h $(LUA_PREFIX)/include/lualib.h
$(CONFIG_LIBRARY): $(CONFIG_OBJECT) Makefile
	rm -f $@
	$(AR) rcs $@ $(CONFIG_OBJECT)

$(BUILD)/session.elf: $(SESSION_OBJECTS) $(CONFIG_LIBRARY) $(LUA_LIBRARY) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(SESSION_OBJECTS) $(CONFIG_LIBRARY) $(LUA_LIBRARY) $(LDLIBS)

$(BUILD)/udp-send.elf $(BUILD)/udp-echo.elf: $(UDP_OBJECT)

$(BUILD)/ping.elf $(BUILD)/tcp.elf $(BUILD)/ttcp.elf: $(DNS_LOOKUP_OBJECT)

$(BUILD)/dig.elf $(BUILD)/ping.elf $(BUILD)/tcp.elf $(BUILD)/ttcp.elf: $(DNS_OBJECTS) $(UDP_OBJECT)

$(BUILD)/shell.elf: $(SHELL_OBJECTS) $(UTILITY_OBJECT)

$(BUILD)/counter.elf: $(COUNTER_OBJECT)

$(BUILD)/ls.elf $(BUILD)/mkdir.elf $(BUILD)/rm.elf $(BUILD)/rmdir.elf $(BUILD)/sync.elf: $(UTILITY_OBJECT)

$(BUILD)/share/hello.txt: hello/message.txt
	@mkdir -p $(@D)
	cp $< $@

# Runtime objects and libraries come from the selected SDK.
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
	@printf '%s\n' "$$SDK" "$$LUA_PREFIX" "$$PICOHTTPPARSER_PREFIX" "$$CC" "$$PYXIS_COMPILER_ID" "$$CPPFLAGS" "$$CFLAGS" "$$LDFLAGS" "$$LDLIBS" > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

FORCE:

clean:
	rm -rf $(BUILD)

.SECONDARY:

-include $(HTTP_OBJECTS:.o=.d) $(PROGRAM_OBJECTS:.o=.d) $(UTILITY_OBJECT:.o=.d) $(UDP_OBJECT:.o=.d) $(DNS_OBJECTS:.o=.d) $(DNS_LOOKUP_OBJECT:.o=.d) $(SHELL_OBJECTS:.o=.d) $(SESSION_OBJECTS:.o=.d) $(CONFIG_OBJECT:.o=.d)
