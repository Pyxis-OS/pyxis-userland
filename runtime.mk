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
               $(BUILD)/lib/console.o $(BUILD)/lib/terminal.o $(BUILD)/lib/handle.o \
               $(BUILD)/lib/file.o $(BUILD)/lib/disk.o $(BUILD)/lib/pipe.o $(BUILD)/lib/endpoint.o $(BUILD)/lib/namespace.o $(BUILD)/lib/provider.o \
               $(BUILD)/lib/directory.o $(BUILD)/lib/path.o $(BUILD)/lib/mount.o \
               $(BUILD)/lib/profile.o $(BUILD)/lib/space.o $(BUILD)/lib/keyboard.o $(BUILD)/lib/pointer.o $(BUILD)/lib/clock.o $(BUILD)/lib/echo.o $(BUILD)/lib/net_config.o $(BUILD)/lib/udp.o $(BUILD)/lib/tcp.o $(BUILD)/lib/random.o $(BUILD)/lib/power.o $(BUILD)/lib/memory.o $(BUILD)/lib/display.o $(BUILD)/lib/process.o \
               $(BUILD)/lib/launcher.o $(BUILD)/lib/program.o $(BUILD)/lib/network_environment.o \
               $(BUILD)/lib/system_info.o $(BUILD)/lib/log.o \
               $(BUILD)/lib/shebang.o $(BUILD)/lib/wait.o
LIBTERM := $(BUILD)/libterm.a
TERM_OBJECTS := $(BUILD)/libterm/term.o $(BUILD)/libterm/key.o $(BUILD)/libterm/line.o
LIBC := $(BUILD)/libc.a
LIBC_SOURCES := $(wildcard libc/*.c)
MUSL_SOURCES := third_party/musl/src/stdio/format_float.c \
                third_party/musl/src/regex/regcomp.c \
                third_party/musl/src/regex/regexec.c \
                third_party/musl/src/regex/regerror.c \
                third_party/musl/src/regex/tre-mem.c \
                third_party/musl/src/math/frexpl.c \
                third_party/musl/src/time/__secs_to_tm.c \
                third_party/musl/src/time/__year_to_secs.c \
                third_party/musl/src/time/__month_to_secs.c \
                third_party/musl/src/time/rule_to_secs.c \
                third_party/musl/src/math/fabs.c \
                third_party/musl/src/math/floor.c \
                third_party/musl/src/math/round.c \
                third_party/musl/src/math/fmod.c \
                third_party/musl/src/math/pow.c \
                third_party/musl/src/math/frexp.c \
                third_party/musl/src/math/ldexp.c \
                third_party/musl/src/math/exp_data.c \
                third_party/musl/src/math/pow_data.c \
                third_party/musl/src/math/__math_xflow.c \
                third_party/musl/src/math/__math_uflow.c \
                third_party/musl/src/math/__math_oflow.c \
                third_party/musl/src/math/__math_invalid.c \
                third_party/musl/src/math/scalbn.c \
                third_party/musl/src/math/scalbnl.c \
                third_party/musl/src/math/ldexpl.c \
                third_party/musl/src/math/fabsl.c \
                third_party/musl/src/math/copysignl.c \
                third_party/musl/src/math/x86_64/fmodl.c \
                third_party/musl/src/math/ceil.c \
                third_party/musl/src/math/x86_64/sqrt.c \
                third_party/musl/src/math/sin.c \
                third_party/musl/src/math/cos.c \
                third_party/musl/src/math/tan.c \
                third_party/musl/src/math/__sin.c \
                third_party/musl/src/math/__cos.c \
                third_party/musl/src/math/__tan.c \
                third_party/musl/src/math/__rem_pio2.c \
                third_party/musl/src/math/__rem_pio2_large.c \
                third_party/musl/src/math/atan.c \
                third_party/musl/src/math/atan2.c \
                third_party/musl/src/internal/floatscan.c
MUSL_OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(MUSL_SOURCES))
LIBC_OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(LIBC_SOURCES)) \
                $(BUILD)/libc/tlsf.o $(BUILD)/libc/setjmp.o $(MUSL_OBJECTS)

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

$(BUILD)/libc/time.o: private CPPFLAGS += -Ithird_party/musl/src/internal

$(MUSL_OBJECTS) $(BUILD)/libc/strtod.o: private CPPFLAGS += -Ithird_party/musl/src/internal
$(MUSL_OBJECTS) $(BUILD)/libc/strtod.o: private CFLAGS += -frounding-math -fexcess-precision=standard

# libm.h forces FP evaluation by writing otherwise unread volatile locals.
$(MUSL_OBJECTS): private CFLAGS += -Wno-unused-but-set-variable

$(BUILD)/third_party/musl/src/time/rule_to_secs.o: private CPPFLAGS += -Ilibc

$(BUILD)/third_party/musl/src/stdio/format_float.o: private CPPFLAGS += -Ilibc
$(BUILD)/third_party/musl/src/stdio/format_float.o: private CFLAGS += -Wno-sign-compare -Wno-parentheses

# Retain upstream's unsigned character tests and ring-index expressions.
$(BUILD)/third_party/musl/src/internal/floatscan.o: private CFLAGS += -Wno-sign-compare -Wno-parentheses

# Retain upstream's sign-bit expression; prec 0-2 from callers always fills fq.
$(BUILD)/third_party/musl/src/math/atan2.o: private CFLAGS += -Wno-parentheses
$(BUILD)/third_party/musl/src/math/__rem_pio2_large.o: private CFLAGS += -Wno-maybe-uninitialized

# Retain TRE's upstream signedness comparisons and intentional parser fallthroughs.
$(BUILD)/third_party/musl/src/regex/regcomp.o $(BUILD)/third_party/musl/src/regex/regexec.o: private CFLAGS += -Wno-sign-compare
$(BUILD)/third_party/musl/src/regex/regcomp.o: private CFLAGS += -Wno-implicit-fallthrough

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
