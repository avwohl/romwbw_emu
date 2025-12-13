# Makefile for CP/M WebAssembly build

EMCC = emcc
CFLAGS = -O2 -std=c++11
LDFLAGS = -s WASM=1 \
          -s EXPORTED_FUNCTIONS='["_main","_cpm_key_input","_cpm_load_system","_cpm_load_bios","_cpm_load_disk","_cpm_load_disk_b","_cpm_load_disk_c","_cpm_create_disk_c","_cpm_start","_cpm_stop","_cpm_get_disk_data","_cpm_get_disk_size","_cpm_get_disk_b_data","_cpm_get_disk_b_size","_cpm_get_disk_c_data","_cpm_get_disk_c_size","_cpm_autostart","_malloc","_free"]' \
          -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","FS"]' \
          -s ALLOW_MEMORY_GROWTH=1 \
          -s INITIAL_MEMORY=16777216 \
          --preload-file bios.sys@/bios.sys \
          --preload-file ../cpm22.sys@/cpm22.sys \
          --preload-file ../drivea.img@/drivea \
          --preload-file drivec.img@/drivec

# RomWBW WebAssembly build flags
ROMWBW_LDFLAGS = -s WASM=1 \
          -s EXPORTED_FUNCTIONS='["_main","_romwbw_key_input","_romwbw_set_boot_string","_romwbw_load_rom","_romwbw_load_disk","_romwbw_get_disk_data","_romwbw_get_disk_size","_romwbw_start","_romwbw_stop","_romwbw_is_running","_romwbw_get_instruction_count","_romwbw_get_pc","_romwbw_set_debug","_romwbw_autostart","_malloc","_free"]' \
          -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","FS"]' \
          -s ALLOW_MEMORY_GROWTH=1 \
          -s INITIAL_MEMORY=67108864

SRCS = cpm_web.cc \
       ../src/qkz80.cc \
       ../src/qkz80_mem.cc \
       ../src/qkz80_reg_set.cc \
       ../src/qkz80_errors.cc

ROMWBW_SRCS = romwbw_web.cc \
              ../src/qkz80.cc \
              ../src/qkz80_mem.cc \
              ../src/qkz80_reg_set.cc \
              ../src/qkz80_errors.cc \
              ../src/hbios_dispatch.cc \
              ../src/emu_io_wasm.cc

all: cpm.js cpm_cli

# Assemble BIOS from source
bios.sys: bios.asm
	um80 -g bios.asm
	ul80 -o bios.sys -S bios.sym -p f600 bios.rel

cpm.js: $(SRCS) bios.sys ../cpm22.sys ../drivea.img
	$(EMCC) $(CFLAGS) $(LDFLAGS) -o cpm.js $(SRCS)

# CLI version for testing
CLI_SRCS = cpm_cli.cc \
           ../src/qkz80.cc \
           ../src/qkz80_mem.cc \
           ../src/qkz80_reg_set.cc \
           ../src/qkz80_errors.cc

cpm_cli: $(CLI_SRCS) bios.sys ../cpm22.sys ../drivea.img
	g++ -O2 -std=c++11 -o cpm_cli $(CLI_SRCS)

# RomWBW WebAssembly build
romwbw.js: $(ROMWBW_SRCS)
	$(EMCC) $(CFLAGS) $(ROMWBW_LDFLAGS) -o romwbw.js $(ROMWBW_SRCS)

# RomWBW with preloaded ROM (optional - requires SBC_simh_std.rom)
romwbw-bundled.js: $(ROMWBW_SRCS) ../SBC_simh_std.rom
	$(EMCC) $(CFLAGS) $(ROMWBW_LDFLAGS) --preload-file ../SBC_simh_std.rom@/romwbw.rom -o romwbw-bundled.js $(ROMWBW_SRCS)

clean:
	rm -f cpm.js cpm.wasm cpm.data bios.sys bios.rel bios.sym cpm_cli
	rm -f romwbw.js romwbw.wasm romwbw-bundled.js romwbw-bundled.wasm romwbw-bundled.data

serve: cpm.js
	python3 -m http.server 8080

serve-romwbw: romwbw.js
	python3 -m http.server 8080

deploy: cpm.js
	mkdir -p ~/www/cpm
	cp index.html cpm.js cpm.wasm cpm.data ~/www/cpm/

deploy-romwbw: romwbw.js
	mkdir -p ~/www/romwbw
	cp romwbw.html romwbw.js romwbw.wasm ~/www/romwbw/

.PHONY: all clean serve serve-romwbw deploy deploy-romwbw
