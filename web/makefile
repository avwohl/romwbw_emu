# Makefile for RomWBW WebAssembly build

# em++, not emcc. Every source here is C++, and emcc no longer implies the
# C++ driver at link time, so linking through it leaves operator new/delete
# and the rest of libc++ undefined:
#   wasm-ld: error: undefined symbol: operator new(unsigned long)
#   emcc: warning: link failed with undefined C++ symbols. Try linking with
#         'em++' or passing '-sDEFAULT_TO_CXX'
# This built until the v1.35 release because CI installs `emsdk latest` and
# the behaviour changed underneath it. Override with EMCC= if you need the
# old driver for something.
EMCC = em++

# qkz80 sources from cpmemu project (for WebAssembly build)
QKZ80_SRC = ../../cpmemu/src

# RomWBW WebAssembly build flags
VERSION := $(shell cat ../VERSION)
ROMWBW_CFLAGS = -O2 -std=c++11 -I$(QKZ80_SRC) -DEMU_VERSION=\"$(VERSION)\"
ROMWBW_LDFLAGS = -s WASM=1 \
          -s EXPORTED_FUNCTIONS='["_main","_romwbw_key_input","_romwbw_set_boot_string","_romwbw_clear_nvram","_romwbw_load_rom","_romwbw_load_disk","_romwbw_get_disk_data","_romwbw_get_disk_size","_romwbw_is_disk_dirty","_romwbw_clear_disk_dirty","_romwbw_set_disk_is_manifest","_romwbw_set_disk_warning_suppressed","_romwbw_poll_manifest_warning","_romwbw_start","_romwbw_stop","_romwbw_is_running","_romwbw_is_waiting","_romwbw_get_instruction_count","_romwbw_get_pc","_romwbw_set_debug","_romwbw_run_batch","_romwbw_autostart","_emu_host_file_load","_emu_host_file_cancel","_malloc","_free"]' \
          -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","FS","HEAPU8"]' \
          -s ALLOW_MEMORY_GROWTH=1 \
          -s INITIAL_MEMORY=67108864

ROMWBW_SRCS = romwbw_web.cc \
              $(QKZ80_SRC)/qkz80.cc \
              $(QKZ80_SRC)/qkz80_mem.cc \
              $(QKZ80_SRC)/qkz80_reg_set.cc \
              $(QKZ80_SRC)/qkz80_errors.cc \
              ../src/hbios_dispatch.cc \
              ../src/hbios_cpu.cc \
              ../src/emu_io_wasm.cc \
              ../src/emu_io_common.cc \
              ../src/emu_init.cc

all: romwbw.js

# RomWBW WebAssembly build
romwbw.js: $(ROMWBW_SRCS)
	$(EMCC) $(ROMWBW_CFLAGS) $(ROMWBW_LDFLAGS) -o romwbw.js $(ROMWBW_SRCS)

# RomWBW debug build with DWARF symbols for Chrome DevTools
romwbw-debug.js: $(ROMWBW_SRCS)
	$(EMCC) $(ROMWBW_CFLAGS) $(ROMWBW_LDFLAGS) -g -gsource-map -o romwbw-debug.js $(ROMWBW_SRCS)

# RomWBW with the ROM preloaded into the Emscripten filesystem (optional).
# This used to preload ../SBC_simh_std.rom: a path that does not exist (the
# stock ROMs live in ../roms/), and a stock hardware ROM, which this emulator
# cannot run at all - it drives hardware that is not emulated and produces no
# output. Preload the emulator ROM instead.
romwbw-bundled.js: $(ROMWBW_SRCS) ../roms/emu_avw.rom
	$(EMCC) $(ROMWBW_CFLAGS) $(ROMWBW_LDFLAGS) --preload-file ../roms/emu_avw.rom@/romwbw.rom -o romwbw-bundled.js $(ROMWBW_SRCS)

clean:
	rm -f romwbw.js romwbw.wasm romwbw-bundled.js romwbw-bundled.wasm romwbw-bundled.data
	rm -f romwbw-debug.js romwbw-debug.wasm romwbw-debug.wasm.map

serve: romwbw.js
	python3 -m http.server 8080

deploy-romwbw-PRODUCTION-ASK-HUMAN-FIRST: romwbw.js
	mkdir -p ~/www/romwbw
	sed "s/@VERSION@/$$(cat ../VERSION)/" romwbw.html-template > ~/www/romwbw/index.html
	cp romwbw.js romwbw.wasm ~/www/romwbw/

# Dev deploy to romwbw1 - safe for Claude to use
deploy-dev: romwbw.js
	mkdir -p ~/www/romwbw1
	sed "s/@VERSION@/$$(cat ../VERSION)-dev $$(date '+%Y-%m-%d %H:%M:%S')/" romwbw.html-template > ~/www/romwbw1/index.html
	cp romwbw.js romwbw.wasm ~/www/romwbw1/
	@echo "Deployed to ~/www/romwbw1/ - NOT production"

.PHONY: all clean serve deploy-romwbw-PRODUCTION-ASK-HUMAN-FIRST deploy-dev
