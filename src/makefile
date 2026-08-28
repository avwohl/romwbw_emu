all: romwbw_emu

# Version comes from the top-level VERSION file (single source of truth).
# Resolve it before -include below so $(lastword $(MAKEFILE_LIST)) is still
# this makefile (robust to make -C src vs make -f src/makefile).
THIS_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
VERSION := $(strip $(shell cat $(THIS_DIR)../VERSION 2>/dev/null))

# Include local.mk if it exists (for machine-specific settings like PKG_CONFIG_PATH)
-include local.mk

# Use pkg-config to find qkz80 library, unless overridden in local.mk
# Set QKZ80_CFLAGS and QKZ80_LIBS in local.mk if qkz80 is not installed system-wide
# Fallback order: pkg-config -> sister directory (../../cpmemu/src) -> /usr/local
# THIS_DIR, not $(dir $(lastword $(MAKEFILE_LIST))): by this point local.mk has
# been included, so the lastword is local.mk, and make resolved that name
# against the CURRENT directory.  The two agree for `make -C src` and for a
# root-level `make -f src/makefile` (where ./local.mk does not exist and so is
# not included at all), which is why nothing has gone wrong yet - but the path
# should not depend on where make was invoked from.  THIS_DIR is captured above
# the -include for exactly this reason.
SISTER_QKZ80_PATH = $(THIS_DIR)../../cpmemu/src
# realpath is empty when the directory does not exist, which is most of the
# time on a machine that needs the warning below, so the tests and the message
# use the unresolved path and only the compiler flags use the resolved one.
SISTER_QKZ80 = $(realpath $(SISTER_QKZ80_PATH))

# Which of the three the build is about to take.  Decided here, above the ?=
# below, because this is the last point at which "the caller chose" can still
# be told apart from "nobody did" - after the ?= they look identical.
#
# The last branch is the one worth interrupting for.  A machine with no
# pkg-config entry and no sibling cpmemu checkout falls through to /usr/local
# and links whatever Z80 core happens to be installed there, which need not be
# the one this tree is developed against.  Nothing failed, nothing was printed,
# and the difference shows up as odd behaviour at run time.  `make qkz80-source`
# prints the answer in all four cases without building anything.
ifeq ($(origin QKZ80_CFLAGS),undefined)
  ifneq ($(shell pkg-config --exists qkz80 2>/dev/null && echo yes),)
    QKZ80_SOURCE = pkg-config ($(shell pkg-config --variable=prefix qkz80 2>/dev/null))
  else ifneq ($(wildcard $(SISTER_QKZ80_PATH)/qkz80.h),)
    QKZ80_SOURCE = the sister checkout, $(SISTER_QKZ80)
  else
    QKZ80_SOURCE = /usr/local - LAST RESORT, nothing else was found
    $(warning qkz80: no pkg-config entry, and no $(SISTER_QKZ80_PATH)/qkz80.h.)
    $(warning qkz80: falling back to -I/usr/local/include/qkz80 -L/usr/local/lib.)
    $(warning qkz80: that is whatever Z80 core is installed there - not)
    $(warning qkz80: necessarily the one this tree is developed against.)
    $(warning qkz80: set QKZ80_CFLAGS and QKZ80_LIBS in src/local.mk to choose.)
  endif
else
  QKZ80_SOURCE = QKZ80_CFLAGS, set by the caller ($(origin QKZ80_CFLAGS))
endif

QKZ80_CFLAGS ?= $(shell pkg-config --cflags qkz80 2>/dev/null || \
    ([ -f "$(SISTER_QKZ80_PATH)/qkz80.h" ] && echo "-I$(SISTER_QKZ80)") || \
    echo "-I/usr/local/include/qkz80")
# The sister-checkout branch names the archive instead of saying
# "-L<dir> -lqkz80", which is what it used to say.  The test above it is
# for libqkz80.a, but -lqkz80 does not have to resolve to the file that was
# tested for: `make libs` in cpmemu leaves a libqkz80.dylib in the same
# directory and the linker prefers it.  Measured here, on macOS 27: with
# both present every test binary linked against the dylib and then died at
# start-up with "Library not loaded: /usr/local/lib/libqkz80.4.dylib",
# because that is the install_name a not-yet-installed dylib carries, and
# DYLD_LIBRARY_PATH cannot rescue it - SIP strips DYLD_* when make spawns
# /bin/sh, so `make test` fails even where the binary run by hand does not.
# Naming the archive makes the link use the file the test found.  Anyone who
# does want the shared library sets QKZ80_LIBS.
QKZ80_LIBS ?= $(shell pkg-config --libs qkz80 2>/dev/null || \
    ([ -f "$(SISTER_QKZ80_PATH)/libqkz80.a" ] && echo "$(SISTER_QKZ80)/libqkz80.a") || \
    echo "-L/usr/local/lib -lqkz80")

# -Wimplicit-int-conversion and -Wshorten-64-to-32 are both Clang-only, and
# GCC does not ignore an unknown -W flag, it fails the compile.  Probing for
# them is not tidiness: hardcoded, an unknown flag stops every GCC build on
# the first object file, which has broken this family's CI before.
# cpmemu/src/makefile guards -Wshorten-64-to-32 the same way, for the same
# reason.
#
# -Wimplicit-int-conversion is the Clang name for what MSVC reports as C4267
# at /W3.  It finds the places a size_t or int index is truncated into a
# 16-bit guest address.  Those truncations are what HBIOS wants - a Z80 doing
# the same pointer arithmetic wraps at 64K - so each site now carries an
# explicit (uint16_t) cast that says so, rather than a suppression.
# -Wconversion is deliberately NOT here: it adds the Z80 core's intended
# mod-256 wraps on top, which is the call cpmemu made too.
#
# Two diagnostics per object file remain that this tree cannot fix: the
# qkz80_MK_INT16 macro narrows its int result to qkz80_uint16, so every file
# that calls set_low()/set_high() reports qkz80_reg_pair.h:32 and :35.  The
# fix is a qkz80_uint16() around that macro body, which lives in cpmemu; this
# repo goes quiet when CPMEMU_REF moves past it.
IMPLICIT_INT_CONVERSION := $(shell $(CXX) -Wimplicit-int-conversion -x c++ -fsyntax-only /dev/null 2>/dev/null && echo -Wimplicit-int-conversion)
SHORTEN_64_TO_32 := $(shell $(CXX) -Wshorten-64-to-32 -x c++ -fsyntax-only /dev/null 2>/dev/null && echo -Wshorten-64-to-32)
CXXFLAGS = -std=c++11 -Wall $(IMPLICIT_INT_CONVERSION) $(SHORTEN_64_TO_32) -O2 -I. $(QKZ80_CFLAGS)
# A missing VERSION file falls back to the in-source default, never an
# empty-string macro
ifneq ($(VERSION),)
CXXFLAGS += -DEMU_VERSION=\"$(VERSION)\"
endif
LDFLAGS ?=
LDLIBS = $(QKZ80_LIBS)

# Use STATIC=1 for static builds (portable across glibc versions)
ifeq ($(STATIC),1)
LDFLAGS += -static
endif

# Object files for romwbw_emu using emu_io abstraction
ROMWBW_OBJS = emu_io_cli.o emu_io_common.o hbios_dispatch.o hbios_cpu.o emu_init.o emu_config.o

# Main emulator (boots RomWBW via HBIOS). version.cc is recompiled inside
# the link recipe so the build date is fresh exactly when the binary changes
# (and no-op makes stay no-ops).
romwbw_emu: romwbw_emu.o $(ROMWBW_OBJS)
	$(CXX) $(CXXFLAGS) -c version.cc -o version.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) romwbw_emu.o version.o $(ROMWBW_OBJS) $(LDLIBS) -o romwbw_emu

# A version bump must recompile the file that prints it (wildcard: a missing
# VERSION file falls back to the in-source default instead of breaking make)
romwbw_emu.o: $(wildcard $(THIS_DIR)../VERSION)

# Pattern rule to compile .cc files into .o object files
%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Tests.  Two kinds, and they cannot share a link line:
#
#   vda_keyboard  drives the HBIOS dispatcher and brings its own emu_io stubs,
#   hbios_hostname  so each links the core objects but NOT emu_io_cli.o - their
#                 stubs would collide with that file's definitions.
#   cli_console   is a test OF emu_io_cli.o, so it links exactly that and
#                 nothing else: the file needs nothing from the rest of the
#                 core.  openpty lives in libutil on Linux and in libc on the
#                 BSDs and macOS.
#   cli_hostfile  is also a test of emu_io_cli.o, plus emu_io_common.o for the
#                 shared path helper the sandboxed backends reduce a path with.
TESTS = vda_keyboard hbios_hostname cli_console cli_hostfile

DISPATCH_OBJS = emu_io_common.o hbios_dispatch.o hbios_cpu.o emu_init.o

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
PTY_LIBS = -lutil
else
PTY_LIBS =
endif

# The web frontend's JS is tested with node when node is present.  It is not a
# build dependency - the wasm needs emcc, which this does not - so a machine
# without node skips it rather than failing.
test: $(addprefix test_,$(TESTS))
	@status=0; \
	for t in $^; do ./$$t || status=1; echo; done; \
	if command -v node >/dev/null 2>&1; then \
	  node $(THIS_DIR)../tests/web_reload_disks.js || status=1; echo; \
	  node $(THIS_DIR)../tests/web_console_output.js || status=1; echo; \
	else \
	  echo "SKIP  web JS tests (node not on PATH)"; echo; \
	fi; \
	sh $(THIS_DIR)../disks/verify_disk_utils.sh || status=1; echo; \
	if [ $$status -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; \
	echo "ALL TESTS PASSED"

test_vda_keyboard: $(THIS_DIR)../tests/vda_keyboard.cc $(DISPATCH_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $< $(DISPATCH_OBJS) $(LDLIBS) -o $@

test_hbios_hostname: $(THIS_DIR)../tests/hbios_hostname.cc $(DISPATCH_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $< $(DISPATCH_OBJS) $(LDLIBS) -o $@

test_cli_console: $(THIS_DIR)../tests/cli_console.cc emu_io_cli.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $< emu_io_cli.o $(PTY_LIBS) -o $@

test_cli_hostfile: $(THIS_DIR)../tests/cli_hostfile.cc emu_io_cli.o emu_io_common.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $< emu_io_cli.o emu_io_common.o -o $@

clean:
	@rm -f romwbw_emu $(addprefix test_,$(TESTS)) *.o *.lst *.ihx *.com *.cdb *.rel *.map *~

# Which qkz80 this tree would build against, without building anything.  The
# fallback chain above is three deep and only its last step says anything on
# its own, so this is how to ask the other three.
qkz80-source:
	@echo "qkz80 comes from: $(QKZ80_SOURCE)"
	@echo "  QKZ80_CFLAGS = $(QKZ80_CFLAGS)"
	@echo "  QKZ80_LIBS   = $(QKZ80_LIBS)"

.PHONY: all test clean install uninstall qkz80-source

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

install: romwbw_emu
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 romwbw_emu $(DESTDIR)$(BINDIR)/romwbw_emu

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/romwbw_emu
