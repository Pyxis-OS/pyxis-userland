SDK ?= build/sdk
LUA ?= lua
SDK := $(abspath $(SDK))
BUILD ?= build/apps
DESTDIR ?= $(BUILD)/install
LUA_PREFIX ?= build/ports-dev/lua
LUA_PREFIX := $(abspath $(LUA_PREFIX))
PICOHTTPPARSER_PREFIX ?= build/ports-dev/picohttpparser
PICOHTTPPARSER_PREFIX := $(abspath $(PICOHTTPPARSER_PREFIX))
MBEDTLS_PREFIX ?= build/ports-dev/mbedtls
MBEDTLS_PREFIX := $(abspath $(MBEDTLS_PREFIX))
ZLIB_PREFIX ?= build/ports-dev/zlib
ZLIB_PREFIX := $(abspath $(ZLIB_PREFIX))
LIBPNG_PREFIX ?= build/ports-dev/libpng
LIBPNG_PREFIX := $(abspath $(LIBPNG_PREFIX))
HTTP_PARSER_LIBRARY := $(PICOHTTPPARSER_PREFIX)/lib/libpicohttpparser.a
LUA_LIBRARY := $(LUA_PREFIX)/lib/liblua.a
ZLIB_LIBRARY := $(ZLIB_PREFIX)/lib/libz.a
LIBPNG_LIBRARY := $(LIBPNG_PREFIX)/lib/libpng.a
INSTALL_PROGRAMS := mux remote-terminal xfer httpfs allocbench iobench ipcbench session boot-init init-install installer shell client server counter textfs cat cp echo head log hostname lspci lsusb ls mkdir rm rmdir mv sync date ping dig tcp ttcp udp-send udp-echo mandelbrot mousetest screenshot pcm
.DEFAULT_GOAL := all

ifneq ($(MAKECMDGOALS),clean)
ifeq ($(wildcard $(SDK)/share/pyxis.mk),)
$(error Missing SDK at $(SDK); supply SDK=/path/to/sdk exported by Pyxis)
endif
include $(SDK)/share/pyxis.mk
-include $(MBEDTLS_PREFIX)/share/mbedtls.mk
endif
CPPFLAGS := $(PYXIS_CPPFLAGS)
CFLAGS := $(PYXIS_CFLAGS)
LDFLAGS := $(PYXIS_LDFLAGS)
LDLIBS := $(PYXIS_LDLIBS)
export LUA_PREFIX PICOHTTPPARSER_PREFIX MBEDTLS_PREFIX ZLIB_PREFIX LIBPNG_PREFIX SDK CC CPPFLAGS CFLAGS LDFLAGS LDLIBS PYXIS_COMPILER_ID

PROGRAM_OBJECTS := $(BUILD)/pcm/main.o $(BUILD)/remote-terminal/main.o $(BUILD)/httpfs/main.o $(BUILD)/allocbench/main.o $(BUILD)/iobench/main.o $(BUILD)/ipcbench/main.o $(BUILD)/hello/main.o $(BUILD)/client/main.o \
                   $(BUILD)/server/main.o $(BUILD)/counter/main.o $(BUILD)/textfs/main.o $(BUILD)/cat/main.o $(BUILD)/cp/main.o $(BUILD)/echo/main.o $(BUILD)/head/main.o $(BUILD)/log/main.o $(BUILD)/hostname/main.o $(BUILD)/lspci/main.o $(BUILD)/lsusb/main.o \
                   $(BUILD)/ls/main.o $(BUILD)/mkdir/main.o \
                   $(BUILD)/rm/main.o $(BUILD)/rmdir/main.o \
                   $(BUILD)/mv/main.o $(BUILD)/sync/main.o $(BUILD)/date/main.o $(BUILD)/ping/main.o \
                   $(BUILD)/udp-send/main.o $(BUILD)/udp-echo/main.o $(BUILD)/dig/main.o $(BUILD)/tcp/main.o \
                   $(BUILD)/ttcp/main.o \
                   $(BUILD)/shell/main.o $(BUILD)/mandelbrot/main.o $(BUILD)/mousetest/main.o $(BUILD)/session/main.o $(BUILD)/init-install/main.o
SHELL_OBJECTS := $(BUILD)/shell/parse.o $(BUILD)/shell/directory.o \
                 $(BUILD)/shell/launch.o $(BUILD)/shell/command.o \
                 $(BUILD)/shell/script.o $(BUILD)/shell/history.o
COUNTER_OBJECT := $(BUILD)/counter/namespace.o
MUX_OBJECTS := $(BUILD)/mux/layout.o $(BUILD)/mux/render.o $(BUILD)/mux/session.o $(BUILD)/mux/emulator.o $(BUILD)/mux/pointer.o $(BUILD)/mux/clipboard.o
REMOTE_OBJECT := $(BUILD)/remote-terminal/session.o
XFER_OBJECTS := $(BUILD)/xfer/main.o $(BUILD)/xfer/protocol.o $(BUILD)/xfer/hash.o
TCP_SERVE_OBJECT := $(BUILD)/tcp/serve.o
IOBENCH_OBJECTS := $(BUILD)/iobench/common.o $(BUILD)/iobench/write.o $(BUILD)/iobench/pipe.o
INSTALLER_OBJECTS := $(addprefix $(BUILD)/installer/,main.o io.o gpt.o pool.o consent.o esp.o esp_read.o programs.o)
NPFS_LIBRARY := $(SDK)/sysroot/usr/lib/libnpfs-format.a
BOOT_INIT_OBJECTS := $(BUILD)/boot-init/main.o $(BUILD)/boot-init/config.o
SESSION_OBJECTS := $(BUILD)/session/main.o $(BUILD)/session/config.o $(BUILD)/session/network.o \
                   $(BUILD)/session/tcp_server.o $(BUILD)/session/remote_server.o $(BUILD)/session/dhcp.o
HTTPFS_OBJECTS := $(BUILD)/httpfs/main.o $(BUILD)/httpfs/trust.o
HTTP_LIBRARY := $(BUILD)/libhttp.a
HTTP_OBJECTS := $(BUILD)/libhttp/uri.o $(BUILD)/libhttp/fetch.o
TLS_LIBRARY := $(BUILD)/libtls.a
TLS_OBJECT := $(BUILD)/libtls/tls.o
TLS_EXPORT_IDENTITY := $(abspath $(BUILD)/.mbedtls-export)
CONFIG_LIBRARY := $(BUILD)/libconfig.a
CONFIG_OBJECT := $(BUILD)/libconfig/config.o
DNS_LOOKUP_OBJECT := $(BUILD)/common/dns_lookup.o
DNS_OBJECTS := $(BUILD)/common/dns_message.o $(BUILD)/common/dns_query.o
UDP_OBJECT := $(BUILD)/common/udp.o
CP_OBJECT := $(BUILD)/cp/copy.o $(BUILD)/cp/tree.o
LS_OBJECTS := $(BUILD)/ls/listing.o $(BUILD)/ls/output.o
SCREENSHOT_OBJECTS := $(BUILD)/screenshot/main.o $(BUILD)/screenshot/png.o
UTILITY_OBJECT := $(BUILD)/common/directory.o

.PHONY: mux remote-terminal xfer all install libhttp libtls httpfs allocbench iobench ipcbench session boot-init init-install installer hello client server counter textfs cat cp echo head log hostname lspci lsusb ls mkdir rm rmdir mv sync date ping dig tcp ttcp udp-send udp-echo shell mandelbrot mousetest screenshot pcm clean FORCE
all: $(INSTALL_PROGRAMS) $(BUILD)/share/hello.txt $(TLS_LIBRARY)

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
	  install -m 644 init/services.sh "$$staging/init-services"; \
	  install -m 644 init/remote.sh "$$staging/init-remote"; \
	  install -m 644 init/remote-services.sh "$$staging/init-remote-services"; \
	  install -m 644 init/installed.sh "$$staging/init-installed"; \
	  install -m 644 config/session.lua "$$staging/config/session.lua"; \
	  install -m 644 config/network.lua "$$staging/config/network.lua"; \
	  install -m 644 config/live.lua "$$staging/config/live.lua"; \
	  install -m 644 config/installed.lua "$$staging/config/installed.lua"; \
	  install -m 644 hello/message.txt "$$staging/share/hello.txt"; \
	  install -m 644 "$(BUILD)/share/iobench.bin" "$$staging/share/iobench.bin"; \
	  install -m 644 "$(BUILD)/share/iobench-small.bin" "$$staging/share/iobench-small.bin"; \
	  if ! diff -qr "$$staging" "$(DESTDIR)" >/dev/null 2>&1; then \
	    rm -rf -- "$(DESTDIR)"; \
	    mv -- "$$staging" "$(DESTDIR)"; \
	  fi

allocbench: $(BUILD)/allocbench.pxe
iobench: $(BUILD)/iobench.pxe $(BUILD)/share/iobench.bin $(BUILD)/share/iobench-small.bin
ipcbench: $(BUILD)/ipcbench.pxe

httpfs: $(BUILD)/httpfs.pxe
libhttp: $(HTTP_LIBRARY)
libtls: $(TLS_LIBRARY)

mux: $(BUILD)/mux.pxe

$(BUILD)/mux.pxe: $(MUX_OBJECTS)

remote-terminal: $(BUILD)/remote-terminal.pxe

xfer: $(BUILD)/xfer.pxe

$(BUILD)/remote-terminal.pxe: $(REMOTE_OBJECT)

session: $(BUILD)/session.pxe
boot-init: $(BUILD)/boot-init.pxe
init-install: $(BUILD)/init-install.pxe
installer: $(BUILD)/installer.pxe
hello: $(BUILD)/hello.pxe $(BUILD)/share/hello.txt
client: $(BUILD)/client.pxe
server: $(BUILD)/server.pxe
counter: $(BUILD)/counter.pxe
textfs: $(BUILD)/textfs.pxe
cat: $(BUILD)/cat.pxe
cp: $(BUILD)/cp.pxe
echo: $(BUILD)/echo.pxe
head: $(BUILD)/head.pxe
log: $(BUILD)/log.pxe
hostname: $(BUILD)/hostname.pxe
lspci: $(BUILD)/lspci.pxe
lsusb: $(BUILD)/lsusb.pxe
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
mousetest: $(BUILD)/mousetest.pxe
screenshot: $(BUILD)/screenshot.pxe
pcm: $(BUILD)/pcm.pxe

$(SCREENSHOT_OBJECTS): private CPPFLAGS += -I$(LIBPNG_PREFIX)/include -I$(ZLIB_PREFIX)/include
$(SCREENSHOT_OBJECTS): $(LIBPNG_PREFIX)/include/png.h $(LIBPNG_PREFIX)/include/pngconf.h $(LIBPNG_PREFIX)/include/pnglibconf.h $(ZLIB_PREFIX)/include/zlib.h $(ZLIB_PREFIX)/include/zconf.h
$(BUILD)/screenshot.pxe: $(SCREENSHOT_OBJECTS) $(UTILITY_OBJECT) $(LIBPNG_LIBRARY) $(ZLIB_LIBRARY) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(SCREENSHOT_OBJECTS) $(UTILITY_OBJECT) $(LIBPNG_LIBRARY) $(ZLIB_LIBRARY) $(LDLIBS)

$(HTTP_OBJECTS): private CPPFLAGS += -I$(PICOHTTPPARSER_PREFIX)/include
$(HTTP_OBJECTS): $(PICOHTTPPARSER_PREFIX)/include/picohttpparser.h
$(HTTP_LIBRARY): $(HTTP_OBJECTS) $(DNS_OBJECTS) $(UDP_OBJECT) Makefile
	rm -f $@
	$(AR) rcs $@ $(HTTP_OBJECTS) $(DNS_OBJECTS) $(UDP_OBJECT)

$(TLS_OBJECT) $(BUILD)/xfer/hash.o: private CPPFLAGS += $(MBEDTLS_CPPFLAGS)
$(BUILD)/xfer/hash.o: $(MBEDTLS_PREFIX)/share/mbedtls.mk $(TLS_EXPORT_IDENTITY)
$(TLS_OBJECT): $(MBEDTLS_PREFIX)/share/mbedtls.mk $(TLS_EXPORT_IDENTITY)
$(TLS_LIBRARY): $(TLS_OBJECT) Makefile
	rm -f $@
	$(AR) rcs $@ $(TLS_OBJECT)

# Ports exports normalize timestamps. Compare content before trusting objects
# built against their configured headers and libraries.
$(TLS_EXPORT_IDENTITY): FORCE
	@mkdir -p $(@D)
	@set -eu; \
	  trap 'rm -f -- "$@.tmp" "$@.files.tmp"' EXIT; \
	  cd "$(MBEDTLS_PREFIX)"; \
	  find . -type f -print0 > "$@.files.tmp"; \
	  LC_ALL=C sort -z "$@.files.tmp" -o "$@.files.tmp"; \
	  xargs -0 sha256sum -- < "$@.files.tmp" > "$@.tmp"; \
	  cmp -s "$@.tmp" "$@" || mv -- "$@.tmp" "$@"

$(BUILD)/xfer.pxe: $(XFER_OBJECTS) $(TLS_EXPORT_IDENTITY) $(MBEDTLS_LIBRARIES) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(XFER_OBJECTS) $(MBEDTLS_LIBRARIES) $(LDLIBS)

$(BUILD)/httpfs.pxe: $(HTTPFS_OBJECTS) $(HTTP_LIBRARY) $(HTTP_PARSER_LIBRARY) $(TLS_LIBRARY) $(TLS_EXPORT_IDENTITY) $(MBEDTLS_LIBRARIES) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(HTTPFS_OBJECTS) $(HTTP_LIBRARY) $(HTTP_PARSER_LIBRARY) $(TLS_LIBRARY) $(MBEDTLS_LIBRARIES) $(LDLIBS)

$(SESSION_OBJECTS) $(BOOT_INIT_OBJECTS) $(CONFIG_OBJECT): private CPPFLAGS += -I$(LUA_PREFIX)/include
$(SESSION_OBJECTS) $(BOOT_INIT_OBJECTS) $(CONFIG_OBJECT): $(LUA_PREFIX)/include/lua.h $(LUA_PREFIX)/include/lauxlib.h $(LUA_PREFIX)/include/luaconf.h $(LUA_PREFIX)/include/lualib.h
$(CONFIG_LIBRARY): $(CONFIG_OBJECT) Makefile
	rm -f $@
	$(AR) rcs $@ $(CONFIG_OBJECT)

$(BUILD)/boot-init.pxe: $(BOOT_INIT_OBJECTS) $(CONFIG_LIBRARY) $(LUA_LIBRARY) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(BOOT_INIT_OBJECTS) $(CONFIG_LIBRARY) $(LUA_LIBRARY) $(LDLIBS)

$(BUILD)/session.pxe: $(SESSION_OBJECTS) $(UDP_OBJECT) $(CONFIG_LIBRARY) $(LUA_LIBRARY) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(SESSION_OBJECTS) $(UDP_OBJECT) $(CONFIG_LIBRARY) $(LUA_LIBRARY) $(LDLIBS)

$(BUILD)/udp-send.pxe $(BUILD)/udp-echo.pxe: $(UDP_OBJECT)

$(BUILD)/ping.pxe $(BUILD)/tcp.pxe $(BUILD)/ttcp.pxe: $(DNS_LOOKUP_OBJECT)

$(BUILD)/tcp.pxe: $(TCP_SERVE_OBJECT)

$(BUILD)/dig.pxe $(BUILD)/ping.pxe $(BUILD)/tcp.pxe $(BUILD)/ttcp.pxe: $(DNS_OBJECTS) $(UDP_OBJECT)

$(BUILD)/shell.pxe: $(SHELL_OBJECTS) $(UTILITY_OBJECT)

$(BUILD)/counter.pxe: $(COUNTER_OBJECT)

$(BUILD)/iobench.pxe: $(IOBENCH_OBJECTS) $(UTILITY_OBJECT)

$(BUILD)/installer.pxe: $(INSTALLER_OBJECTS) $(NPFS_LIBRARY) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(INSTALLER_OBJECTS) $(NPFS_LIBRARY) $(LDLIBS)

$(BUILD)/cp.pxe: $(CP_OBJECT) $(UTILITY_OBJECT)

$(BUILD)/ls.pxe: $(LS_OBJECTS)

$(BUILD)/ls.pxe $(BUILD)/mkdir.pxe $(BUILD)/rm.pxe $(BUILD)/rmdir.pxe $(BUILD)/sync.pxe: $(UTILITY_OBJECT)

$(BUILD)/share/hello.txt: hello/message.txt
	@mkdir -p $(@D)
	cp $< $@

$(BUILD)/share/iobench.bin: iobench/fixture.lua Makefile
	@mkdir -p $(@D)
	$(LUA) $< > $@.tmp
	mv $@.tmp $@

$(BUILD)/share/iobench-small.bin: iobench/fixture.lua Makefile
	@mkdir -p $(@D)
	$(LUA) $< 32768 > $@.tmp
	mv $@.tmp $@

# Runtime objects and libraries come from the selected SDK.
$(BUILD)/%.pxe: $(BUILD)/%/main.o $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(SDK)/share/pyxis.mk $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(filter %.o,$^) $(LDLIBS)

$(BUILD)/%.o: %.c Makefile $(SDK)/share/pyxis.mk $(BUILD)/.config
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# SDK selection and compiler flags are build inputs even if files are older.
$(BUILD)/.config: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' "$$SDK" "$$LUA_PREFIX" "$$PICOHTTPPARSER_PREFIX" "$$MBEDTLS_PREFIX" "$$ZLIB_PREFIX" "$$LIBPNG_PREFIX" "$$CC" "$$PYXIS_COMPILER_ID" "$$CPPFLAGS" "$$CFLAGS" "$$LDFLAGS" "$$LDLIBS" > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

FORCE:

clean:
	rm -rf $(BUILD)

.SECONDARY:

-include $(MUX_OBJECTS:.o=.d) $(BUILD)/mux/main.d $(REMOTE_OBJECT:.o=.d) $(HTTPFS_OBJECTS:.o=.d) $(COUNTER_OBJECT:.o=.d) $(TLS_OBJECT:.o=.d) $(HTTP_OBJECTS:.o=.d) $(PROGRAM_OBJECTS:.o=.d) $(UTILITY_OBJECT:.o=.d) $(UDP_OBJECT:.o=.d) $(DNS_OBJECTS:.o=.d) $(DNS_LOOKUP_OBJECT:.o=.d) $(SHELL_OBJECTS:.o=.d) $(SESSION_OBJECTS:.o=.d) $(BOOT_INIT_OBJECTS:.o=.d) $(CONFIG_OBJECT:.o=.d) $(IOBENCH_OBJECTS:.o=.d) $(TCP_SERVE_OBJECT:.o=.d)
-include $(INSTALLER_OBJECTS:.o=.d) $(XFER_OBJECTS:.o=.d) $(LS_OBJECTS:.o=.d) $(CP_OBJECT:.o=.d) $(SCREENSHOT_OBJECTS:.o=.d)
