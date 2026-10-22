INSTALLER_PUBLICATION_SCRATCH ?= 1
export INSTALLER_PUBLICATION_SCRATCH
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
WPA_RSN_PREFIX ?= build/ports-dev/wpa-supplicant
WPA_RSN_PREFIX := $(abspath $(WPA_RSN_PREFIX))
HTTP_PARSER_LIBRARY := $(PICOHTTPPARSER_PREFIX)/lib/libpicohttpparser.a
LUA_LIBRARY := $(LUA_PREFIX)/lib/liblua.a
ZLIB_LIBRARY := $(ZLIB_PREFIX)/lib/libz.a
LIBPNG_LIBRARY := $(LIBPNG_PREFIX)/lib/libpng.a
INSTALL_PROGRAMS := wifi-supplicant mux remote-terminal xfer http httpfs http-stage allocbench iobench ipcbench threadprobe session boot-init init-install init-fat32-inspect init-fat32-mount init-fat32-mount-write fat32-inspect init-fat32-write fat32-write installer shell client server counter textfs cat cp echo head log hostname lspci lsusb ls acpidump mkdir rm rmdir mv ln tree sync date sleep ping dig tcp ttcp udp-send udp-echo mandelbrot mousetest pointer-min screenshot screenshot-saver pcm
.DEFAULT_GOAL := all

ifneq ($(MAKECMDGOALS),clean)
ifeq ($(wildcard $(SDK)/share/pyxis.mk),)
$(error Missing SDK at $(SDK); supply SDK=/path/to/sdk exported by Pyxis)
endif
include $(SDK)/share/pyxis.mk
-include $(MBEDTLS_PREFIX)/share/mbedtls.mk
-include $(WPA_RSN_PREFIX)/share/wpa-rsn.mk
endif
CPPFLAGS := $(PYXIS_CPPFLAGS)
CFLAGS := $(PYXIS_CFLAGS)
LDFLAGS := $(PYXIS_LDFLAGS)
LDLIBS := $(PYXIS_LDLIBS)
THREADPROBE_CXX_CPPFLAGS := $(PYXIS_CXX_CPPFLAGS)
THREADPROBE_CXXFLAGS := $(PYXIS_CXXFLAGS) -fno-exceptions -fno-rtti -ftls-model=local-exec
export CXX THREADPROBE_CXX_CPPFLAGS THREADPROBE_CXXFLAGS
export LUA_PREFIX PICOHTTPPARSER_PREFIX MBEDTLS_PREFIX ZLIB_PREFIX LIBPNG_PREFIX SDK CC CPPFLAGS CFLAGS LDFLAGS LDLIBS PYXIS_COMPILER_ID

PROGRAM_OBJECTS := $(BUILD)/http/main.o $(BUILD)/pcm/main.o $(BUILD)/remote-terminal/main.o $(BUILD)/httpfs/main.o $(BUILD)/allocbench/main.o $(BUILD)/iobench/main.o $(BUILD)/ipcbench/main.o $(BUILD)/threadprobe/main.o $(BUILD)/hello/main.o $(BUILD)/client/main.o \
                   $(BUILD)/server/main.o $(BUILD)/counter/main.o $(BUILD)/textfs/main.o $(BUILD)/cat/main.o $(BUILD)/cp/main.o $(BUILD)/echo/main.o $(BUILD)/head/main.o $(BUILD)/log/main.o $(BUILD)/hostname/main.o $(BUILD)/lspci/main.o $(BUILD)/lsusb/main.o $(BUILD)/acpidump/main.o \
                   $(BUILD)/ls/main.o $(BUILD)/mkdir/main.o \
                   $(BUILD)/rm/main.o $(BUILD)/rmdir/main.o \
                   $(BUILD)/mv/main.o $(BUILD)/ln/main.o $(BUILD)/tree/main.o $(BUILD)/sync/main.o $(BUILD)/date/main.o $(BUILD)/sleep/main.o $(BUILD)/ping/main.o \
                   $(BUILD)/udp-send/main.o $(BUILD)/udp-echo/main.o $(BUILD)/dig/main.o $(BUILD)/tcp/main.o \
                   $(BUILD)/ttcp/main.o \
                   $(BUILD)/shell/main.o $(BUILD)/mandelbrot/main.o $(BUILD)/mousetest/main.o $(BUILD)/examples/pointer-min/main.o $(BUILD)/examples/font-min/main.o $(BUILD)/session/main.o $(BUILD)/init-install/main.o \
                   $(BUILD)/init-fat32-inspect/main.o $(BUILD)/init-fat32-mount/main.o $(BUILD)/fat32-inspect/main.o \
                   $(BUILD)/init-fat32-write/main.o $(BUILD)/fat32-write/main.o $(BUILD)/fat32ctl/main.o $(BUILD)/net/main.o $(BUILD)/wifi/main.o
SHELL_OBJECTS := $(BUILD)/shell/parse.o $(BUILD)/shell/capture.o $(BUILD)/shell/expand.o $(BUILD)/shell/values.o $(BUILD)/shell/environment.o $(BUILD)/shell/directory.o \
                 $(BUILD)/shell/launch.o $(BUILD)/shell/command.o $(BUILD)/shell/fat32.o \
                 $(BUILD)/shell/script.o $(BUILD)/shell/history.o $(BUILD)/shell/settings.o $(BUILD)/shell/complete.o \
                 $(BUILD)/shell/complete_path.o $(BUILD)/common/pane_cwd.o
COUNTER_OBJECT := $(BUILD)/counter/environment.o $(BUILD)/counter/forwarders.o
MUX_OBJECTS := $(BUILD)/mux/layout.o $(BUILD)/mux/render.o $(BUILD)/mux/session.o $(BUILD)/mux/emulator.o $(BUILD)/mux/pointer.o $(BUILD)/mux/clipboard.o $(BUILD)/common/pane_cwd.o $(BUILD)/common/pane_broker.o
REMOTE_OBJECT := $(BUILD)/remote-terminal/session.o $(BUILD)/common/pane_broker.o
XFER_OBJECTS := $(BUILD)/xfer/main.o $(BUILD)/xfer/protocol.o $(BUILD)/xfer/hash.o
TCP_SERVE_OBJECT := $(BUILD)/tcp/serve.o
IOBENCH_OBJECTS := $(BUILD)/iobench/common.o $(BUILD)/iobench/write.o $(BUILD)/iobench/pipe.o
INSTALLER_OBJECTS := $(addprefix $(BUILD)/installer/,main.o battery.o gpt.o pool.o consent.o esp.o esp_read.o programs.o manifest.o plan_record.o plan.o executor.o coexist.o review.o esp_activate.o coexist_ui.o target.o)
NPFS_LIBRARY := $(SDK)/sysroot/usr/lib/libnpfs-format.a
FONT_LIBRARY := $(SDK)/sysroot/usr/lib/libfont.a
FAT32_LIBRARY := $(SDK)/sysroot/usr/lib/libfat32-read.a
FAT32_WRITE_LIBRARY := $(SDK)/sysroot/usr/lib/libfat32-write.a
BOOT_INIT_OBJECTS := $(BUILD)/boot-init/main.o $(BUILD)/boot-init/network.o $(BUILD)/pkg/maintenance.o $(BUILD)/pkg/store.o
SESSION_OBJECTS := $(BUILD)/session/wifi_supplicant.o $(BUILD)/session/main.o $(BUILD)/session/config.o $(BUILD)/session/retention.o $(BUILD)/session/screenshot.o $(BUILD)/session/network.o $(BUILD)/common/system_log.o \
                   $(BUILD)/session/tcp_server.o $(BUILD)/session/remote_server.o $(BUILD)/session/dhcp.o $(BUILD)/boot-init/network.o
HTTPFS_OBJECTS := $(BUILD)/httpfs/main.o $(BUILD)/httpfs/trust.o $(BUILD)/httpfs/provider.o
HTTP_LIBRARY := $(BUILD)/libhttp.a
HTTP_OBJECTS := $(BUILD)/libhttp/uri.o $(BUILD)/libhttp/fetch.o $(BUILD)/libhttp/request.o
TLS_LIBRARY := $(BUILD)/libtls.a
TLS_OBJECT := $(BUILD)/libtls/tls.o
TLS_EXPORT_IDENTITY := $(abspath $(BUILD)/.mbedtls-export)
CONFIG_LIBRARY := $(BUILD)/libconfig.a
CONFIG_OBJECT := $(addprefix $(BUILD)/libconfig/,config.o boot.o session.o network.o selection.o handoff.o)
CONFIG_EDITOR_OBJECTS := $(addprefix $(BUILD)/config-editor/,main.o context.o catalog.o ownership.o render.o plan.o replay.o storage.o record.o) $(BUILD)/installer/esp_transfer.o $(BUILD)/installer/pending_archive.o $(BUILD)/installer/io.o $(BUILD)/config-editor/readiness.o
INSTALLER_BOOT_OBJECTS := $(addprefix $(BUILD)/config-editor/,ownership.o catalog.o storage.o record.o render.o)
INSTALLER_CONFIG_OBJECTS := $(addprefix $(BUILD)/installer/,publication_frontend.o installed_publication.o installed_probe.o)
# The shared boot publisher (installer/boot_publisher.h). The installer links
# this archive; front ends add their own target admission and consent.
PUBLISHER_LIBRARY := $(BUILD)/libpublisher.a
PUBLISHER_OBJECTS := $(addprefix $(BUILD)/installer/,boot_publisher.o configuration.o publication.o publication_snapshot.o pending.o pending_archive.o recovery.o esp_payload.o esp_transfer.o esp_directory.o esp_view.o boot_entry.o boot_archive.o archive_config.o digest.o progress.o io.o) \
    $(addprefix $(BUILD)/config-editor/,catalog.o ownership.o render.o plan.o storage.o record.o readiness.o)
# Discovery borrows the installer validators. Separate sectioned objects let
# the selector retain only read-only inspection from the mixed private modules.
INSTALL_SELECTOR_SOURCES := init-install/update.c installer/io.c installer/gpt.c \
    installer/consent.c installer/esp_read.c installer/review.c installer/coexist.c \
    installer/progress.c installer/digest.c installer/installed_probe.c installer/boot_entry.c \
    installer/pending.c installer/plan_record.c \
    config-editor/record.c config-editor/storage.c config-editor/ownership.c \
    config-editor/catalog.c xfer/hash.c
INSTALL_SELECTOR_OBJECTS := $(addprefix $(BUILD)/init-install/inspect/,$(INSTALL_SELECTOR_SOURCES:.c=.o))
BOOT_INIT_OBJECTS += $(BUILD)/boot-init/handoff.o $(BUILD)/config-editor/record.o $(BUILD)/installer/digest.o $(BUILD)/xfer/hash.o
BOOT_INIT_LIBRARIES := $(MBEDTLS_LIBRARIES)
PKG_OBJECTS := $(addprefix $(BUILD)/pkg/,main.o archive.o metadata.o fetch.o store.o commands.o progress.o)
CONFIG_REVISION := $(shell git rev-parse --short=12 HEAD)
DNS_LOOKUP_OBJECT := $(BUILD)/common/dns_lookup.o
DNS_OBJECTS := $(BUILD)/common/dns_message.o $(BUILD)/common/dns_query.o
UDP_OBJECT := $(BUILD)/common/udp.o
CP_OBJECT := $(BUILD)/cp/copy.o $(BUILD)/cp/tree.o
LS_OBJECTS := $(BUILD)/ls/listing.o $(BUILD)/ls/output.o
SCREENSHOT_OBJECTS := $(BUILD)/screenshot/main.o $(BUILD)/screenshot/png.o
SCREENSHOT_SAVER_OBJECTS := $(BUILD)/screenshot/saver.o $(BUILD)/screenshot/png.o
UTILITY_OBJECT := $(BUILD)/common/directory.o

.PHONY: mux remote-terminal xfer http all install libhttp libtls httpfs allocbench iobench ipcbench threadprobe session boot-init init-install init-fat32-inspect init-fat32-mount init-fat32-mount-write fat32-inspect init-fat32-write fat32-write installer hello client server counter textfs cat cp echo head log hostname lspci lsusb ls acpidump mkdir rm rmdir mv ln tree sync date sleep ping dig tcp ttcp udp-send udp-echo shell mandelbrot mousetest pointer-min font-min screenshot screenshot-saver pcm clean FORCE
.PHONY: fat32ctl net wifi config pkg
all: $(INSTALL_PROGRAMS) font-min fat32ctl net wifi config pkg $(BUILD)/shell.pxb/manifest.json $(BUILD)/httpfs.pxb $(BUILD)/share/hello.txt $(TLS_LIBRARY)

# Lua scripts for hand-run native checks, installed as boot://share/checks/.
CHECK_SCRIPTS := console-burst.lua console-flood.lua cpu-busy.lua exit-latency.lua

# Publish only the boot payload, never objects or debug ELFs. Recreate it so
# removed programs/assets cannot survive from an earlier install.
install: all
	@set -eu; \
	  staging="$(DESTDIR).tmp"; \
	  trap 'rm -rf -- "$$staging"' EXIT; \
	  rm -rf -- "$$staging"; \
	  mkdir -p "$$staging/share/checks" "$$staging/share/examples/nvim" "$$staging/config"; \
	  for program in $(INSTALL_PROGRAMS); do \
	    name=$$program; \
	    if [ "$$program" = shell ]; then name=init-shell; fi; \
	    install -m 644 "$(BUILD)/$$program.pxe" "$$staging/$$name.pxe"; \
	  done; \
	  cp -a "$(BUILD)/fat32ctl.pxb" "$(BUILD)/net.pxb" "$(BUILD)/wifi.pxb" "$(BUILD)/httpfs.pxb" "$(BUILD)/config.pxb" "$(BUILD)/pkg.pxb" "$(BUILD)/shell.pxb" "$(BUILD)/font-min.pxb" "$$staging/"; \
	  install -m 644 init/development.pxs "$$staging/init"; \
	  install -m 644 init/readonly.pxs "$$staging/init-readonly"; \
	  install -m 644 init/services.pxs "$$staging/init-services"; \
	  install -m 644 init/remote.pxs "$$staging/init-remote"; \
	  install -m 644 init/remote-services.pxs "$$staging/init-remote-services"; \
	  mkdir -p "$$staging/share/licenses/wpa-rsn"; \
	  cp -a "$(WPA_RSN_PREFIX)/share/licenses/wpa-rsn/." "$$staging/share/licenses/wpa-rsn/"; \
	  install -m 644 config/session.lua "$$staging/config/session.lua"; \
	  install -m 644 config/network.lua "$$staging/config/network.lua"; \
	  install -m 644 config/live.lua "$$staging/config/live.lua"; \
	  mkdir -p "$$staging/share/installer/system"; \
	  install -m 644 config/system/boot.lua "$$staging/share/installer/system/boot.lua"; \
	  install -m 644 config/session.lua "$$staging/share/installer/system/session.lua"; \
	  install -m 644 config/network.lua "$$staging/share/installer/system/network.lua"; \
	  install -m 644 init/installed.pxs "$$staging/share/installer/system/init-installed"; \
	  install -m 644 config/installed.lua "$$staging/config/installed.lua"; \
	  install -m 644 config/packages.lua "$$staging/config/packages.lua"; \
	  install -m 644 "$(BUILD)/config-editor/catalog.lua" "$$staging/config/catalog.lua"; \
	  install -m 644 hello/message.txt "$$staging/share/hello.txt"; \
	  install -m 644 examples/pointer-min/main.c "$$staging/share/examples/pointer-min.c"; \
	  install -m 644 examples/font-min/main.c "$$staging/share/examples/font-min.c"; \
	  install -m 644 examples/font-min/manifest.json "$$staging/share/examples/font-min-manifest.json"; \
	  install -m 644 examples/nvim/init.lua "$$staging/share/examples/nvim/init.lua"; \
	  install -m 644 "$(BUILD)/share/iobench.bin" "$$staging/share/iobench.bin"; \
	  install -m 644 "$(BUILD)/share/iobench-small.bin" "$$staging/share/iobench-small.bin"; \
	  for script in $(CHECK_SCRIPTS); do \
	    install -m 644 "checks/$$script" "$$staging/share/checks/$$script"; \
	  done; \
	  if ! diff -qr "$$staging" "$(DESTDIR)" >/dev/null 2>&1; then \
	    rm -rf -- "$(DESTDIR)"; \
	    mv -- "$$staging" "$(DESTDIR)"; \
	  fi

allocbench: $(BUILD)/allocbench.pxe
iobench: $(BUILD)/iobench.pxe $(BUILD)/share/iobench.bin $(BUILD)/share/iobench-small.bin
ipcbench: $(BUILD)/ipcbench.pxe
threadprobe: $(BUILD)/threadprobe.pxe

$(BUILD)/threadprobe.pxe: $(BUILD)/threadprobe/main.o $(BUILD)/threadprobe/runtime.o $(BUILD)/threadprobe/runtime_io.o $(BUILD)/threadprobe/runtime_cxx.o $(BUILD)/threadprobe/entry.o $(BUILD)/threadprobe/tls_objects.o $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_CXX_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(SDK)/share/pyxis.mk $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(filter %.o,$^) $(PYXIS_CXX_LDLIBS)

$(BUILD)/threadprobe/runtime_cxx.o: threadprobe/runtime_cxx.cpp Makefile $(SDK)/share/pyxis.mk $(BUILD)/.config
	@mkdir -p $(@D)
	$(CXX) $(PYXIS_CXX_CPPFLAGS) $(PYXIS_CXXFLAGS) -ftls-model=local-exec -c $< -o $@

# This helper uses language TLS without the shared C++ standard library profile.
$(BUILD)/threadprobe/tls_objects.o: threadprobe/tls_objects.cpp Makefile $(SDK)/share/pyxis.mk $(BUILD)/.config
	@mkdir -p $(@D)
	$(CXX) $(THREADPROBE_CXX_CPPFLAGS) $(THREADPROBE_CXXFLAGS) -c $< -o $@

$(BUILD)/threadprobe/entry.o: threadprobe/entry.S Makefile $(SDK)/share/pyxis.mk $(BUILD)/.config
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

http: $(BUILD)/http.pxe
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
init-fat32-inspect: $(BUILD)/init-fat32-inspect.pxe
init-fat32-mount: $(BUILD)/init-fat32-mount.pxe
init-fat32-mount-write: $(BUILD)/init-fat32-mount-write.pxe

$(BUILD)/init-fat32-mount-write.pxe: $(BUILD)/init-fat32-mount.pxe Makefile
	cp $< $@
fat32-inspect: $(BUILD)/fat32-inspect.pxe
init-fat32-write: $(BUILD)/init-fat32-write.pxe
fat32-write: $(BUILD)/fat32-write.pxe
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
acpidump: $(BUILD)/acpidump.pxe
ls: $(BUILD)/ls.pxe
mkdir: $(BUILD)/mkdir.pxe
rm: $(BUILD)/rm.pxe
rmdir: $(BUILD)/rmdir.pxe
mv: $(BUILD)/mv.pxe
ln: $(BUILD)/ln.pxe
tree: $(BUILD)/tree.pxe
sync: $(BUILD)/sync.pxe
date: $(BUILD)/date.pxe
sleep: $(BUILD)/sleep.pxe
ping: $(BUILD)/ping.pxe
dig: $(BUILD)/dig.pxe
tcp: $(BUILD)/tcp.pxe
ttcp: $(BUILD)/ttcp.pxe
udp-send: $(BUILD)/udp-send.pxe
udp-echo: $(BUILD)/udp-echo.pxe
shell: $(BUILD)/shell.pxe
fat32ctl: $(BUILD)/fat32ctl.pxb/manifest.json
net: $(BUILD)/net.pxb/manifest.json
wifi: $(BUILD)/wifi.pxb/manifest.json
config: $(BUILD)/config.pxb/manifest.json
mandelbrot: $(BUILD)/mandelbrot.pxe
mousetest: $(BUILD)/mousetest.pxe
pointer-min: $(BUILD)/pointer-min.pxe
font-min: $(BUILD)/font-min.pxb/manifest.json
screenshot: $(BUILD)/screenshot.pxe
screenshot-saver: $(BUILD)/screenshot-saver.pxe
pcm: $(BUILD)/pcm.pxe

$(SCREENSHOT_OBJECTS) $(SCREENSHOT_SAVER_OBJECTS): private CPPFLAGS += -I$(LIBPNG_PREFIX)/include -I$(ZLIB_PREFIX)/include
$(SCREENSHOT_OBJECTS) $(SCREENSHOT_SAVER_OBJECTS): $(LIBPNG_PREFIX)/include/png.h $(LIBPNG_PREFIX)/include/pngconf.h $(LIBPNG_PREFIX)/include/pnglibconf.h $(ZLIB_PREFIX)/include/zlib.h $(ZLIB_PREFIX)/include/zconf.h
$(BUILD)/screenshot.pxe: $(SCREENSHOT_OBJECTS) $(UTILITY_OBJECT) $(LIBPNG_LIBRARY) $(ZLIB_LIBRARY) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(SCREENSHOT_OBJECTS) $(UTILITY_OBJECT) $(LIBPNG_LIBRARY) $(ZLIB_LIBRARY) $(LDLIBS)

$(BUILD)/screenshot-saver.pxe: $(SCREENSHOT_SAVER_OBJECTS) $(UTILITY_OBJECT) $(LIBPNG_LIBRARY) $(ZLIB_LIBRARY) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(SCREENSHOT_SAVER_OBJECTS) $(UTILITY_OBJECT) $(LIBPNG_LIBRARY) $(ZLIB_LIBRARY) $(LDLIBS)

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

$(SESSION_OBJECTS) $(BOOT_INIT_OBJECTS) $(CONFIG_OBJECT) $(BUILD)/wifi/profile.o $(BUILD)/installer/boot_entry.o $(BUILD)/installer/programs.o: private CPPFLAGS += -I$(LUA_PREFIX)/include
$(SESSION_OBJECTS) $(BOOT_INIT_OBJECTS) $(CONFIG_OBJECT) $(BUILD)/wifi/profile.o $(BUILD)/installer/boot_entry.o $(BUILD)/installer/programs.o: $(LUA_PREFIX)/include/lua.h $(LUA_PREFIX)/include/lauxlib.h $(LUA_PREFIX)/include/luaconf.h $(LUA_PREFIX)/include/lualib.h
$(CONFIG_LIBRARY): $(CONFIG_OBJECT) Makefile
	rm -f $@
	$(AR) rcs $@ $(CONFIG_OBJECT)

$(PUBLISHER_LIBRARY): $(PUBLISHER_OBJECTS) Makefile
	rm -f $@
	$(AR) rcs $@ $(PUBLISHER_OBJECTS)

$(BUILD)/boot-init.pxe: $(BOOT_INIT_LIBRARIES) $(TLS_EXPORT_IDENTITY) $(BOOT_INIT_OBJECTS) $(CONFIG_LIBRARY) $(LUA_LIBRARY) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(BOOT_INIT_OBJECTS) $(CONFIG_LIBRARY) $(LUA_LIBRARY) $(BOOT_INIT_LIBRARIES) $(LDLIBS)

$(BUILD)/session.pxe: $(SESSION_OBJECTS) $(UDP_OBJECT) $(CONFIG_LIBRARY) $(LUA_LIBRARY) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(SESSION_OBJECTS) $(UDP_OBJECT) $(CONFIG_LIBRARY) $(LUA_LIBRARY) $(LDLIBS)

$(BUILD)/udp-send.pxe $(BUILD)/udp-echo.pxe: $(UDP_OBJECT)

$(BUILD)/ping.pxe $(BUILD)/tcp.pxe $(BUILD)/ttcp.pxe: $(DNS_LOOKUP_OBJECT)

$(BUILD)/tcp.pxe: $(TCP_SERVE_OBJECT)

$(BUILD)/dig.pxe $(BUILD)/ping.pxe $(BUILD)/tcp.pxe $(BUILD)/ttcp.pxe: $(DNS_OBJECTS) $(UDP_OBJECT)

$(BUILD)/shell.pxe: $(SHELL_OBJECTS) $(UTILITY_OBJECT)

$(BUILD)/shell.pxb/manifest.json: $(BUILD)/shell.pxe shell/manifest.json shell/README.md
	@rm -rf "$(@D)"
	@mkdir -p "$(@D)/app/bin"
	install -m 644 $(BUILD)/shell.pxe "$(@D)/app/bin/"
	install -m 644 shell/README.md "$(@D)/app/"
	install -m 644 shell/manifest.json "$@"

$(BUILD)/fat32ctl.pxb/manifest.json: $(BUILD)/fat32ctl.pxe fat32ctl/manifest.json fat32ctl/README.md
	@rm -rf "$(@D)"
	@mkdir -p "$(@D)/app/bin"
	install -m 644 $(BUILD)/fat32ctl.pxe "$(@D)/app/bin/"
	install -m 644 fat32ctl/README.md "$(@D)/app/"
	install -m 644 fat32ctl/manifest.json "$@"

$(BUILD)/net.pxb/manifest.json: $(BUILD)/net.pxe net/manifest.json net/README.md
	@rm -rf "$(@D)"
	@mkdir -p "$(@D)/app/bin"
	install -m 644 $(BUILD)/net.pxe "$(@D)/app/bin/"
	install -m 644 net/README.md "$(@D)/app/"
	install -m 644 net/manifest.json "$@"

WIFI_SUPPLICANT_OBJECTS := $(addprefix $(BUILD)/wifi-supplicant/,main.o rsn.o os.o)
$(WIFI_SUPPLICANT_OBJECTS): private CPPFLAGS += $(WPA_RSN_CPPFLAGS)
$(WIFI_SUPPLICANT_OBJECTS): private CFLAGS += -Wno-unused-parameter
$(WIFI_SUPPLICANT_OBJECTS): $(WPA_RSN_PREFIX)/share/wpa-rsn.mk
wifi-supplicant: $(BUILD)/wifi-supplicant.pxe
$(BUILD)/wifi-supplicant.pxe: $(WIFI_SUPPLICANT_OBJECTS) $(WPA_RSN_LIBRARIES) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(WIFI_SUPPLICANT_OBJECTS) $(WPA_RSN_LIBRARIES) $(LDLIBS)

$(BUILD)/wifi.pxe: $(BUILD)/wifi/main.o $(BUILD)/wifi/profile.o $(CONFIG_LIBRARY) $(LUA_LIBRARY) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(BUILD)/wifi/main.o $(BUILD)/wifi/profile.o $(CONFIG_LIBRARY) $(LUA_LIBRARY) $(LDLIBS)

$(BUILD)/wifi.pxb/manifest.json: $(BUILD)/wifi.pxe wifi/manifest.json wifi/README.md
	@rm -rf "$(@D)"
	@mkdir -p "$(@D)/app/bin"
	install -m 644 $(BUILD)/wifi.pxe "$(@D)/app/bin/"
	install -m 644 wifi/README.md "$(@D)/app/"
	install -m 644 wifi/manifest.json "$@"

$(BUILD)/boot-init/handoff.o $(CONFIG_EDITOR_OBJECTS) $(INSTALLER_CONFIG_OBJECTS) $(PUBLISHER_OBJECTS): private CPPFLAGS += -I$(LUA_PREFIX)/include $(MBEDTLS_CPPFLAGS)
$(CONFIG_EDITOR_OBJECTS) $(INSTALLER_CONFIG_OBJECTS) $(PUBLISHER_OBJECTS): $(LUA_PREFIX)/include/lua.h $(MBEDTLS_PREFIX)/share/mbedtls.mk $(TLS_EXPORT_IDENTITY)

$(BUILD)/config.pxe: $(NPFS_LIBRARY) $(BUILD)/installer/publication.o $(BUILD)/installer/esp_payload.o $(BUILD)/installer/esp_directory.o $(CONFIG_EDITOR_OBJECTS) $(CONFIG_LIBRARY) $(LUA_LIBRARY) $(BUILD)/installer/digest.o $(BUILD)/installer/progress.o $(BUILD)/xfer/hash.o $(TLS_EXPORT_IDENTITY) $(MBEDTLS_LIBRARIES) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(CONFIG_EDITOR_OBJECTS) $(NPFS_LIBRARY) $(BUILD)/installer/publication.o $(BUILD)/installer/esp_payload.o $(BUILD)/installer/esp_directory.o $(CONFIG_LIBRARY) $(LUA_LIBRARY) $(BUILD)/installer/digest.o $(BUILD)/installer/progress.o $(BUILD)/xfer/hash.o $(MBEDTLS_LIBRARIES) $(LDLIBS)

$(BUILD)/config-editor/catalog.lua: scripts/config-catalog.lua config-editor/catalog-source.lua config/installed.lua config/session.lua config/network.lua FORCE
	@mkdir -p $(@D)
	$(LUA) scripts/config-catalog.lua '$(CONFIG_REVISION)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

$(BUILD)/config.pxb/manifest.json: $(BUILD)/config.pxe $(BUILD)/config-editor/catalog.lua config-editor/manifest.json config-editor/README.md config/installed.lua config/session.lua config/network.lua
	@rm -rf "$(@D)"
	@mkdir -p "$(@D)/app/bin" "$(@D)/app/defaults"
	install -m 644 $(BUILD)/config.pxe "$(@D)/app/bin/"
	install -m 644 config/installed.lua config/session.lua config/network.lua $(BUILD)/config-editor/catalog.lua "$(@D)/app/defaults/"
	install -m 644 config-editor/README.md "$(@D)/app/"
	install -m 644 config-editor/manifest.json "$@"

$(BUILD)/counter.pxe: $(COUNTER_OBJECT)

$(BUILD)/iobench.pxe: $(IOBENCH_OBJECTS) $(UTILITY_OBJECT)

$(BUILD)/installer/plan.o $(BUILD)/installer/executor.o $(BUILD)/installer/target.o $(BUILD)/installer/esp_read.o $(BUILD)/installer/installed_publication.o $(BUILD)/installer/installed_probe.o $(BUILD)/installer/esp_view.o $(BUILD)/installer/review.o $(BUILD)/installer/main.o $(BUILD)/init-install/main.o: private CPPFLAGS += -DINSTALLER_PUBLICATION_SCRATCH=$(INSTALLER_PUBLICATION_SCRATCH)

$(INSTALL_SELECTOR_OBJECTS): private CPPFLAGS += -DINSTALLER_PUBLICATION_SCRATCH=$(INSTALLER_PUBLICATION_SCRATCH) -I$(LUA_PREFIX)/include $(MBEDTLS_CPPFLAGS)
$(INSTALL_SELECTOR_OBJECTS): private CFLAGS += -ffunction-sections -fdata-sections
$(INSTALL_SELECTOR_OBJECTS): $(MBEDTLS_PREFIX)/share/mbedtls.mk $(TLS_EXPORT_IDENTITY)

$(BUILD)/init-install/inspect/%.o: %.c Makefile $(SDK)/share/pyxis.mk $(BUILD)/.config
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/init-install.pxe: $(BUILD)/init-install/main.o $(INSTALL_SELECTOR_OBJECTS) $(FAT32_LIBRARY) $(NPFS_LIBRARY) $(TLS_EXPORT_IDENTITY) $(MBEDTLS_LIBRARIES) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -Wl,--gc-sections -o $@ $(PYXIS_START) $(BUILD)/init-install/main.o $(INSTALL_SELECTOR_OBJECTS) $(FAT32_LIBRARY) $(NPFS_LIBRARY) $(MBEDTLS_LIBRARIES) $(LDLIBS)

$(INSTALLER_OBJECTS): private CPPFLAGS += -I$(LUA_PREFIX)/include $(MBEDTLS_CPPFLAGS)
$(INSTALLER_OBJECTS): $(MBEDTLS_PREFIX)/share/mbedtls.mk $(TLS_EXPORT_IDENTITY)

$(BUILD)/installer.pxe: $(INSTALLER_CONFIG_OBJECTS) $(PUBLISHER_LIBRARY) $(CONFIG_LIBRARY) $(LUA_LIBRARY) $(FAT32_LIBRARY) $(INSTALLER_OBJECTS) $(BUILD)/xfer/hash.o $(TLS_EXPORT_IDENTITY) $(MBEDTLS_LIBRARIES) $(NPFS_LIBRARY) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(INSTALLER_OBJECTS) $(INSTALLER_CONFIG_OBJECTS) $(PUBLISHER_LIBRARY) $(CONFIG_LIBRARY) $(LUA_LIBRARY) $(FAT32_LIBRARY) $(BUILD)/xfer/hash.o $(NPFS_LIBRARY) $(MBEDTLS_LIBRARIES) $(LDLIBS)

$(BUILD)/fat32-inspect/main.o: $(SDK)/sysroot/usr/include/pyxis_fs/fat32.h
$(BUILD)/fat32-inspect.pxe: $(BUILD)/fat32-inspect/main.o $(FAT32_LIBRARY) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(SDK)/share/pyxis.mk $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(BUILD)/fat32-inspect/main.o $(FAT32_LIBRARY) $(LDLIBS)

$(BUILD)/fat32-write/main.o: $(SDK)/sysroot/usr/include/pyxis_fs/fat32.h $(SDK)/sysroot/usr/include/pyxis_fs/fat32_write.h
$(BUILD)/fat32-write.pxe: $(BUILD)/fat32-write/main.o $(FAT32_WRITE_LIBRARY) $(FAT32_LIBRARY) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(SDK)/share/pyxis.mk $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(BUILD)/fat32-write/main.o $(FAT32_WRITE_LIBRARY) $(FAT32_LIBRARY) $(LDLIBS)

$(BUILD)/cp.pxe: $(CP_OBJECT) $(UTILITY_OBJECT)

$(BUILD)/ls.pxe $(BUILD)/tree.pxe: $(LS_OBJECTS)

$(BUILD)/ls.pxe $(BUILD)/acpidump.pxe $(BUILD)/tree.pxe $(BUILD)/mkdir.pxe $(BUILD)/rm.pxe $(BUILD)/rmdir.pxe $(BUILD)/sync.pxe $(BUILD)/ln.pxe: $(UTILITY_OBJECT)

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

# The example lives under examples/ so it is not mistaken for an installed tool.
$(BUILD)/pointer-min.pxe: $(BUILD)/examples/pointer-min/main.o $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(SDK)/share/pyxis.mk $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(filter %.o,$^) $(LDLIBS)

$(BUILD)/font-min.pxb/manifest.json: $(BUILD)/font-min.pxe examples/font-min/manifest.json
	@rm -rf "$(@D)"
	@mkdir -p "$(@D)/app/bin"
	install -m 644 $(BUILD)/font-min.pxe "$(@D)/app/bin/"
	install -m 644 examples/font-min/manifest.json "$@"

# font-min links libfont, which is not one of the default libraries.
$(BUILD)/font-min.pxe: $(BUILD)/examples/font-min/main.o $(FONT_LIBRARY) $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(SDK)/share/pyxis.mk $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(filter %.o,$^) $(FONT_LIBRARY) $(LDLIBS)

# Runtime objects and libraries come from the selected SDK.
$(BUILD)/%.pxe: $(BUILD)/%/main.o $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(SDK)/share/pyxis.mk $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(filter %.o,$^) $(LDLIBS)

$(BUILD)/%.o: %.c Makefile $(SDK)/share/pyxis.mk $(BUILD)/.config
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# SDK selection and compiler flags are build inputs even if files are older.
$(BUILD)/.config: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' "$$SDK" "$$LUA_PREFIX" "$$PICOHTTPPARSER_PREFIX" "$$MBEDTLS_PREFIX" "$$ZLIB_PREFIX" "$$LIBPNG_PREFIX" "$$CC" "$$CXX" "$$THREADPROBE_CXX_CPPFLAGS" "$$THREADPROBE_CXXFLAGS" "$$PYXIS_COMPILER_ID" "$$CPPFLAGS" "$$CFLAGS" "$$LDFLAGS" "$$LDLIBS" "$$INSTALLER_PUBLICATION_SCRATCH" > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

FORCE:

clean:
	rm -rf $(BUILD)

.SECONDARY:

-include $(MUX_OBJECTS:.o=.d) $(BUILD)/mux/main.d $(REMOTE_OBJECT:.o=.d) $(HTTPFS_OBJECTS:.o=.d) $(COUNTER_OBJECT:.o=.d) $(TLS_OBJECT:.o=.d) $(HTTP_OBJECTS:.o=.d) $(PROGRAM_OBJECTS:.o=.d) $(UTILITY_OBJECT:.o=.d) $(UDP_OBJECT:.o=.d) $(DNS_OBJECTS:.o=.d) $(DNS_LOOKUP_OBJECT:.o=.d) $(SHELL_OBJECTS:.o=.d) $(SESSION_OBJECTS:.o=.d) $(BOOT_INIT_OBJECTS:.o=.d) $(CONFIG_OBJECT:.o=.d) $(IOBENCH_OBJECTS:.o=.d) $(TCP_SERVE_OBJECT:.o=.d)
-include $(INSTALLER_OBJECTS:.o=.d) $(XFER_OBJECTS:.o=.d) $(LS_OBJECTS:.o=.d) $(CP_OBJECT:.o=.d) $(SCREENSHOT_OBJECTS:.o=.d) $(SCREENSHOT_SAVER_OBJECTS:.o=.d)
-include $(CONFIG_EDITOR_OBJECTS:.o=.d)
-include $(PKG_OBJECTS:.o=.d)
-include $(INSTALL_SELECTOR_OBJECTS:.o=.d) $(BUILD)/installer/installed_probe.d

-include $(BUILD)/threadprobe/tls_objects.d $(BUILD)/threadprobe/runtime_cxx.d

http-stage: $(BUILD)/http-stage.pxe

$(BUILD)/http-stage.pxe: $(BUILD)/http-stage/main.o $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(SDK)/share/pyxis.mk $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(filter %.o,$^) $(LDLIBS)

$(BUILD)/httpfs.pxb: $(BUILD)/httpfs.pxe httpfs/manifest.json
	@rm -rf "$@.tmp"
	@mkdir -p "$@.tmp/app/bin"
	@install -m 644 "$<" "$@.tmp/app/bin/httpfs.pxe"
	@install -m 644 httpfs/manifest.json "$@.tmp/manifest.json"
	@rm -rf "$@"
	@mv "$@.tmp" "$@"

pkg: $(BUILD)/pkg.pxb/manifest.json

$(BUILD)/pkg.pxe: $(PKG_OBJECTS) $(BUILD)/xfer/hash.o $(PYXIS_START) $(PYXIS_LIBRARIES) $(PYXIS_LINKER_SCRIPT) Makefile $(BUILD)/.config
	$(CC) $(LDFLAGS) -o $@ $(PYXIS_START) $(PKG_OBJECTS) $(BUILD)/xfer/hash.o $(MBEDTLS_LIBRARIES) $(LDLIBS)

$(BUILD)/pkg.pxb/manifest.json: $(BUILD)/pkg.pxe pkg/manifest.json pkg/README.md
	@mkdir -p $(BUILD)/pkg.pxb/app/bin
	install -m 755 $(BUILD)/pkg.pxe $(BUILD)/pkg.pxb/app/bin/pkg.pxe
	install -m 644 pkg/README.md $(BUILD)/pkg.pxb/app/README.md
	install -m 644 pkg/manifest.json $@
-include $(INSTALLER_CONFIG_OBJECTS:.o=.d) $(PUBLISHER_OBJECTS:.o=.d)
