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
          -s EXPORTED_FUNCTIONS='["_main","_romwbw_key_input","_romwbw_set_boot_string","_romwbw_nvram_changed","_romwbw_get_boot_string","_romwbw_clear_nvram","_romwbw_load_rom","_romwbw_load_disk","_romwbw_get_disk_data","_romwbw_get_disk_size","_romwbw_is_disk_dirty","_romwbw_clear_disk_dirty","_romwbw_set_disk_is_manifest","_romwbw_set_disk_warning_suppressed","_romwbw_poll_manifest_warning","_romwbw_start","_romwbw_stop","_romwbw_is_running","_romwbw_is_waiting","_romwbw_get_instruction_count","_romwbw_get_pc","_romwbw_set_debug","_romwbw_run_batch","_emu_host_file_load","_emu_host_file_cancel","_malloc","_free"]' \
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

# What the page asks the core, asked here.  `_romwbw_nvram_changed` and
# `_romwbw_get_boot_string` are the boot option the guest may have changed
# under it.  Nothing else in this repository builds a wasm, so a rename, a
# dropped EMSCRIPTEN_KEEPALIVE or a typo in EXPORTED_FUNCTIONS would otherwise
# surface only when a release is cut - and the failure is not loud: the page
# silently stops reading NVRAM back, which is the bug that export exists to
# fix.  --require, so this cannot pass by skipping the way it does under
# `make -C src test`.
#
# `_romwbw_supported_releases` was checked here too, until the core stopped
# having an opinion about which RomWBW releases it can boot.
#
# NODE, and not a bare `node`.  emsdk_env.sh puts the emsdk ROOT on PATH, and
# `emsdk/node` is a DIRECTORY holding the interpreter several levels down
# ($EMSDK_NODE names it).  So under a sourced emsdk environment a bare `node`
# resolves to that directory and the shell answers "node: Permission denied",
# which make reports as error 127 - "command not found" - for a node that is
# installed and works.  That is exactly how this target failed the first time
# CI ever ran it, cutting v1.44.  Prefer $EMSDK_NODE when the environment sets
# it, which is precisely when the hazard exists.
NODE ?= $(if $(EMSDK_NODE),$(EMSDK_NODE),node)

check: romwbw.js
	$(NODE) ../tests/web_nvram_boot.js --require

# RomWBW debug build with DWARF symbols for Chrome DevTools
romwbw-debug.js: $(ROMWBW_SRCS)
	$(EMCC) $(ROMWBW_CFLAGS) $(ROMWBW_LDFLAGS) -g -gsource-map -o romwbw-debug.js $(ROMWBW_SRCS)

# romwbw-bundled.js is gone.  It preloaded ../roms/emu_avw.rom into the
# emscripten filesystem for romwbw_autostart() to find; this repository tracks
# no ROM any more, so its prerequisite does not exist, and baking a 512 KB ROM
# into a .data file is the thing that made adding a new one a release of this
# repository.  The page fetches from the mirror instead - see `mirror` below.

clean:
	rm -f romwbw.js romwbw.wasm romwbw.html
	rm -f romwbw-debug.js romwbw-debug.wasm romwbw-debug.wasm.map

# --- the catalog mirror -------------------------------------------------------
#
# A browser cannot fetch a romwbw_disks release asset: those URLs send no
# access-control-allow-origin on either redirect hop and the CORS preflight
# 404s, and the images are release assets only, so the CORS-capable raw.github
# and jsdelivr copies 404 on them.  So the page reads its ROM and disk lists
# from catalog/manifest.json on its own origin, and this is what writes it.
#
# This is also what makes a newly published ROM reach the page with no release
# of this repository: re-run a deploy target and the manifest is regenerated
# from whatever the index now names.  Nothing in the page or the wasm has to
# change.
GET = ../tools/romwbw-get
# Every published release is mirrored.  This used to be --versions=runnable,
# which asked ../src/romwbw_emu which releases it had been compiled to allow
# and mirrored only those; no build has an opinion about that any more.
MIRROR_FLAGS ?= --versions=all
# The default disk set is everything, so a disk published upstream appears
# without anyone editing an allowlist here.  Both releases with every disk is
# roughly 460 MB; set MIRROR_ONLY to trim a small host, e.g.
#   make mirror-dev MIRROR_ONLY=--only=emu_avw,hd1k_combo
MIRROR_ONLY ?=

# romwbw.html is RENDERED, not tracked: romwbw.html-template carries @VERSION@
# and .gitignore has web/romwbw.html.  Until now nothing but the two deploy
# targets did the substitution, so `make serve` served a directory with the
# wasm in it and no page to load it - and MANUAL_CHECKS.md's browser check,
# which starts with `make serve`, could not be carried out as written.
romwbw.html: romwbw.html-template ../VERSION
	sed "s/@VERSION@/$$(cat ../VERSION)-local/" romwbw.html-template > $@

serve: romwbw.js romwbw.html
	$(GET) mirror . $(MIRROR_FLAGS) --only=emu_avw,hd1k_combo
	python3 -m http.server 8080

# vendor/ carries xterm - the page loads it from vendor/, not from a CDN,
# so a deploy that leaves it out is a page with no terminal in it.
VENDOR = vendor/xterm.css vendor/xterm.js vendor/xterm-addon-fit.js \
         vendor/LICENSE.xterm vendor/LICENSE.xterm-addon-fit

mirror-prod:
	$(GET) mirror ~/www/romwbw $(MIRROR_FLAGS) $(MIRROR_ONLY)

mirror-dev:
	$(GET) mirror ~/www/romwbw1 $(MIRROR_FLAGS) $(MIRROR_ONLY)

deploy-romwbw-PRODUCTION-ASK-HUMAN-FIRST: romwbw.js mirror-prod
	mkdir -p ~/www/romwbw/vendor
	sed "s/@VERSION@/$$(cat ../VERSION)/" romwbw.html-template > ~/www/romwbw/index.html
	cp romwbw.js romwbw.wasm ~/www/romwbw/
	cp $(VENDOR) ~/www/romwbw/vendor/

# Dev deploy to romwbw1 - safe for Claude to use
deploy-dev: romwbw.js mirror-dev
	mkdir -p ~/www/romwbw1/vendor
	sed "s/@VERSION@/$$(cat ../VERSION)-dev $$(date '+%Y-%m-%d %H:%M:%S')/" romwbw.html-template > ~/www/romwbw1/index.html
	cp romwbw.js romwbw.wasm ~/www/romwbw1/
	cp $(VENDOR) ~/www/romwbw1/vendor/
	@echo "Deployed to ~/www/romwbw1/ - NOT production"

.PHONY: all check clean serve mirror-prod mirror-dev \
        deploy-romwbw-PRODUCTION-ASK-HUMAN-FIRST deploy-dev
