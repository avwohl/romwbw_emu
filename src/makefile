all: romwbw_emu

CXXFLAGS = -std=c++11 -Wall -O2 -I.
LDFLAGS ?=

# Use STATIC=1 for static builds (portable across glibc versions)
ifeq ($(STATIC),1)
LDFLAGS += -static
endif

# Object files for romwbw_emu using emu_io abstraction
ROMWBW_OBJS = qkz80.o qkz80_errors.o qkz80_mem.o qkz80_reg_set.o emu_io_cli.o

# Main emulator (boots RomWBW via HBIOS)
romwbw_emu: romwbw_emu.o $(ROMWBW_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) romwbw_emu.o $(ROMWBW_OBJS) -o romwbw_emu

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
