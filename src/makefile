all: romwbw_emu

# Include local.mk if it exists (for machine-specific settings like PKG_CONFIG_PATH)
-include local.mk

# Use pkg-config to find qkz80 library, unless overridden in local.mk
# Set QKZ80_CFLAGS and QKZ80_LIBS in local.mk if qkz80 is not installed system-wide
QKZ80_CFLAGS ?= $(shell pkg-config --cflags qkz80 2>/dev/null || echo "-I/usr/local/include/qkz80")
QKZ80_LIBS ?= $(shell pkg-config --libs qkz80 2>/dev/null || echo "-L/usr/local/lib -lqkz80")

CXXFLAGS = -std=c++11 -Wall -O2 -I. $(QKZ80_CFLAGS)
LDFLAGS ?=
LDLIBS = $(QKZ80_LIBS)

# Use STATIC=1 for static builds (portable across glibc versions)
ifeq ($(STATIC),1)
LDFLAGS += -static
endif

# Object files for romwbw_emu using emu_io abstraction
ROMWBW_OBJS = emu_io_cli.o hbios_dispatch.o

# Main emulator (boots RomWBW via HBIOS)
romwbw_emu: romwbw_emu.o $(ROMWBW_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) romwbw_emu.o $(ROMWBW_OBJS) $(LDLIBS) -o romwbw_emu

# Pattern rule to compile .cc files into .o object files
%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@rm -f romwbw_emu *.o *.lst *.ihx *.com *.cdb *.rel *.map *~

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

install: romwbw_emu
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 romwbw_emu $(DESTDIR)$(BINDIR)/romwbw_emu

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/romwbw_emu
