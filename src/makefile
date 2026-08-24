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
SISTER_QKZ80 = $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../../cpmemu/src)
QKZ80_CFLAGS ?= $(shell pkg-config --cflags qkz80 2>/dev/null || \
    ([ -f "$(SISTER_QKZ80)/qkz80.h" ] && echo "-I$(SISTER_QKZ80)") || \
    echo "-I/usr/local/include/qkz80")
QKZ80_LIBS ?= $(shell pkg-config --libs qkz80 2>/dev/null || \
    ([ -f "$(SISTER_QKZ80)/libqkz80.a" ] && echo "-L$(SISTER_QKZ80) -lqkz80") || \
    echo "-L/usr/local/lib -lqkz80")

CXXFLAGS = -std=c++11 -Wall -O2 -I. $(QKZ80_CFLAGS)
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
#                 so it links the core objects but NOT emu_io_cli.o - its stubs
#                 would collide with that file's definitions.
#   cli_console   is a test OF emu_io_cli.o, so it links exactly that and
#                 nothing else: the file needs nothing from the rest of the
#                 core.  openpty lives in libutil on Linux and in libc on the
#                 BSDs and macOS.
TESTS = vda_keyboard cli_console cli_hostfile

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
	else \
	  echo "SKIP  web JS tests (node not on PATH)"; echo; \
	fi; \
	sh $(THIS_DIR)../disks/verify_disk_utils.sh || status=1; echo; \
	if [ $$status -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; \
	echo "ALL TESTS PASSED"

test_vda_keyboard: $(THIS_DIR)../tests/vda_keyboard.cc $(DISPATCH_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $< $(DISPATCH_OBJS) $(LDLIBS) -o $@

test_cli_console: $(THIS_DIR)../tests/cli_console.cc emu_io_cli.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $< emu_io_cli.o $(PTY_LIBS) -o $@

test_cli_hostfile: $(THIS_DIR)../tests/cli_hostfile.cc emu_io_cli.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $< emu_io_cli.o -o $@

clean:
	@rm -f romwbw_emu $(addprefix test_,$(TESTS)) *.o *.lst *.ihx *.com *.cdb *.rel *.map *~

.PHONY: all test clean install uninstall

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

install: romwbw_emu
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 romwbw_emu $(DESTDIR)$(BINDIR)/romwbw_emu

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/romwbw_emu
