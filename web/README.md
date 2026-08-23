# RomWBW WebAssembly Frontend

Browser frontend for the RomWBW emulator. The shared C++ engine is compiled
to WebAssembly with Emscripten and driven from a single-page terminal UI
built on xterm.js 5.3.0 (plus the fit addon 0.8.0), both loaded from the
jsdelivr CDN by `romwbw.html-template`.

Keyboard input is `term.onData`: xterm.js has already resolved the keystroke to
bytes - control keys as their native byte, special keys as their escape
sequence - and the page forwards them to `_romwbw_key_input` unchanged,
dropping only code points above 0xFF. The page installs no key handler of its
own for anything xterm.js already translates, so every Ctrl-letter belongs to
the guest, and the terminal is focused after `term.open()` so the browser does
not get them first. The one handler it does install covers `Ctrl+Shift`+letter,
which xterm.js does not translate *or* cancel: it delivers the plain control
byte and calls `preventDefault()`, except for the combinations the browser owns
(`Ctrl+Shift+V` paste, and the devtools and tab/window shortcuts, most of which
a page cannot cancel anyway). Note that `Terminal.input()` does not exist in
the pinned 5.3.0 - it landed later - so synthetic bytes have to go through the
page's own `sendToGuest()`.

The build shares the core engine sources from `../src`:
`hbios_dispatch.cc`, `hbios_cpu.cc`, `emu_io_wasm.cc`, `emu_io_common.cc`,
and `emu_init.cc`. The qkz80 Z80/8080 CPU core comes from a sibling checkout
of the cpmemu project: the makefile hardcodes `QKZ80_SRC = ../../cpmemu/src`
and pulls `qkz80.cc`, `qkz80_mem.cc`, `qkz80_reg_set.cc`, and
`qkz80_errors.cc` from there, so a cpmemu checkout next to this repo is a
build prerequisite.

HBIOS is implemented entirely in C++ (see
[../docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md)); there is no Z80 driver
code in the web build and no assembler is needed to build it. For the
emulator as a whole, start with [../README.md](../README.md).

## Building

Prerequisites: `emcc` (Emscripten), GNU make, and the sibling cpmemu
checkout at `../../cpmemu` described above.

Makefile targets:

- `make` or `make romwbw.js` - main build; produces `romwbw.js` and
  `romwbw.wasm`.
- `make romwbw-debug.js` - same sources built with `-g -gsource-map`
  (DWARF symbols plus source map) for Chrome DevTools debugging; pairs with
  `romwbw-debug.html` and the `debug_wasm.js` Puppeteer harness.
- `make romwbw-bundled.js` - variant with the ROM preloaded into the
  Emscripten filesystem as `/romwbw.rom` via `--preload-file`. It uses the
  checked-in `../roms/emu_avw.rom`; used together with the
  `_romwbw_autostart` export.
- `make clean` - remove the built .js/.wasm/.data/.map outputs.
- `make serve` - build and serve this directory with
  `python3 -m http.server 8080` for local testing.
- `make deploy-dev` - deploy to `~/www/romwbw1`. Safe to run without
  asking; the version string is stamped with `-dev` and a timestamp.
- `make deploy-romwbw-PRODUCTION-ASK-HUMAN-FIRST` - deploy to the
  production directory `~/www/romwbw`. **Do not run this without explicit
  human approval - the target name is the policy.**

Versioning: the makefile reads `../VERSION` and passes it as
`-DEMU_VERSION` at compile time. Both deploy targets generate `index.html`
by sed-substituting every `@VERSION@` in `romwbw.html-template` with that
version (the template also uses it as a `romwbw.js?v=...` cache-buster).
The deploy targets copy only `index.html`, `romwbw.js`, and `romwbw.wasm`;
the ROM and disk images offered by the page's dropdowns must already be
present in the deploy directory.

Notable Emscripten settings: `ALLOW_MEMORY_GROWTH=1`, 64MB initial memory,
and exported runtime methods `ccall`, `cwrap`, `FS`, `HEAPU8`.

## Exported JavaScript API

These are the `EXPORTED_FUNCTIONS` from the makefile. Most are defined in
`romwbw_web.cc`; the two `_emu_host_file_*` entries live in
`../src/emu_io_wasm.cc`.

- `_main` - Emscripten entry point; initializes the I/O layer and registers
  the main loop.
- `_romwbw_key_input(ch)` - queue one input byte (LF is converted to CR)
  and clear the waiting-for-input state.
- `_romwbw_set_boot_string(str)` - set the NVRAM autoboot string ("2" for
  disk unit 2, "2.3" for unit 2 slice 3, "C" for a ROM app; empty clears
  autoboot so the boot menu shows).
- `_romwbw_clear_nvram()` - clear the NVRAM boot configuration.
- `_romwbw_load_rom(ptr, size)` - create a fresh emulator state and load a
  ROM image from a buffer; returns 0 on success, -1 on failure.
- `_romwbw_load_disk(unit, ptr, size)` - load a disk image into unit 0-15;
  returns 0 on success, -1 on failure.
- `_romwbw_get_disk_data(unit)` / `_romwbw_get_disk_size(unit)` - pointer
  into the WASM heap and size of a loaded disk's in-memory image, used by
  the page's download buttons.
- `_romwbw_is_disk_dirty(unit)` / `_romwbw_clear_disk_dirty(unit)` - new in
  v1.34: per-unit dirty tracking of guest writes that have not been
  downloaded, used for the tab-close warning; downloading a disk clears its
  flag.
- `_romwbw_set_disk_is_manifest(unit, flag)` - mark a unit as a
  server-provided (manifest) disk; see below.
- `_romwbw_set_disk_warning_suppressed(unit, flag)` - per-disk "don't warn"
  suppression for the manifest write warning.
- `_romwbw_poll_manifest_warning()` - returns 1 once per session on the
  first guest write to a manifest disk.
- `_romwbw_start()` - reset the CPU, compute slice counts and drive-letter
  assignment for the loaded disks, and begin execution at ROM address 0.
- `_romwbw_stop()` - halt execution.
- `_romwbw_is_running()` / `_romwbw_is_waiting()` - status queries (running
  flag; waiting for input).
- `_romwbw_get_instruction_count()` / `_romwbw_get_pc()` - counters used by
  the page's debug monitor.
- `_romwbw_set_debug(enable)` - toggle debug logging in the engine, memory
  system, and HBIOS dispatch.
- `_romwbw_run_batch()` - run a single instruction batch (used by test
  harnesses).
- `_romwbw_autostart()` - boot from files preloaded into the Emscripten
  filesystem (`/romwbw.rom`, optional `/hd0.img`); for the bundled build.
- `_emu_host_file_load(ptr, size)` - provide the picked file's bytes to a
  pending R8 host-file read.
- `_emu_host_file_cancel()` - cancel a pending R8 host-file read.
- `_malloc` / `_free` - standard heap helpers for passing buffers.

### Module callbacks

The page supplies callbacks on the Emscripten `Module` object; the engine
invokes them via `EM_JS` shims in `../src/emu_io_wasm.cc`:

- `Module.onConsoleOutput(ch)` - one console output character.
- `Module.onStatus(msg)` - status line text.
- `Module.onLog(msg)` - debug log text (the template defines this before
  `romwbw.js` loads so early output is captured/suppressed).
- `Module.onError(msg)`, `Module.onPrinterOutput(ch)` - optional.
- `Module.onHostFileRequestRead(suggestedName)` - R8 asked to read a host
  file; the page opens a file picker and answers with
  `_emu_host_file_load` or `_emu_host_file_cancel`.
- `Module.onHostFileDownload(filename, blob)` - W8 closed a written file;
  the page triggers a browser download.
- `Module.onVideoClear()`, `Module.onVideoSetCursor(row, col)`,
  `Module.onVideoWriteChar(ch)` - VDA output, emitted from the HBIOS VDA
  handlers.
- `Module.onDskyHex`, `Module.onDskySegments`, `Module.onDskyLeds`,
  `Module.onDskyBeep` - optional DSKY front-panel callbacks.

Note: `romwbw.html-template` additionally defines `Module.onVdaInit`,
`onVdaClear`, `onVdaSetCursor`, `onVdaWriteChar`, `onVdaFill`,
`onVdaScroll`, `onVdaSetAttr`, `onVdaSetColor` and `Module.onSndReset`,
`onSndBeep`, `onSndNote`, `onSndVolume` handlers. The engine does not
currently emit callbacks under those names (VDA output goes through the
`onVideo*` trio above), so they are forward-looking page-side handlers.

## Runtime model

WebAssembly in a browser cannot block, so the emulator state constructor
calls `hbios.setBlockingAllowed(false)`. `main()` registers a main loop
with `emscripten_set_main_loop`; each tick runs a batch of up to 50,000
instructions (`run_batch` in `romwbw_web.cc`), flushing pending console
output to `onConsoleOutput` before and after. The batch loop exits early
whenever the guest is waiting for input, and the main loop simply yields
until `_romwbw_key_input` delivers a byte.

New in v1.34, an R8 host-file read is a second wait state: when the guest
requests a host file, execution pauses until the page answers the file
picker with `_emu_host_file_load` (data) or `_emu_host_file_cancel`
(picker dismissed), after which a cancelled R8 sees EOF and aborts
cleanly. Each batch also runs the engine's shared periodic disk flush
(every 20 seconds while writes are pending).

Persistence:

- NVRAM and disk images live only in WASM memory. Nothing about the guest
  survives a page reload - closing the tab discards all disk writes.
- The one exception is the UI itself: since v1.34 the control selections
  (ROM choice, disk 0/1 selections, slice overrides, boot string, and the
  per-disk "don't warn" checkboxes) persist in browser `localStorage`.
  Local file uploads cannot be restored (browsers forbid programmatic
  file-input values) and the Debug checkbox is deliberately session-only.
- A `beforeunload` handler warns before the tab closes if any disk unit
  has guest writes that have not been downloaded, using
  `_romwbw_is_disk_dirty`. Downloading a disk clears its dirty flag, and
  dirty state survives Stop.

## Manifest disks and the write warning

Disks fetched from the page's server dropdowns are flagged with
`_romwbw_set_disk_is_manifest(unit, 1)`; local uploads are not. While the
emulator runs, the page polls `_romwbw_poll_manifest_warning()` every
500 ms; the first guest write to a manifest disk raises a one-time modal
explaining that changes are in-memory only. Each disk row has a "Don't
warn" checkbox wired to `_romwbw_set_disk_warning_suppressed` (and
persisted with the other UI settings). Modified disks can be saved with
the per-disk download buttons, which read the image via
`_romwbw_get_disk_data` / `_romwbw_get_disk_size` and then clear the
dirty flag.

## Files in this directory

Build and page sources:

- `makefile` - build, serve, and deploy targets described above.
- `romwbw_web.cc` - WASM entry point, emulator state, main loop, and the
  exported functions.
- `romwbw.html-template` - the web page; `@VERSION@` is substituted at
  deploy time to produce `index.html`.
- `romwbw.js` / `romwbw.wasm` - local build outputs of `make romwbw.js`
  (gitignored, not in the repository).

Debug harnesses:

- `romwbw-debug.html` - standalone debug page for the debug build.
- `debug_wasm.js` - Puppeteer headless-Chrome harness that serves this
  directory locally and drives the emulator.

Node test scripts (run with `node`):

- `test_load.js` - minimal module-load smoke test.
- `test_quick.js` - spawn-and-kill quick test with a 3 second timeout.
- `test_rom.js` - ROM boot test driving `_romwbw_run_batch` directly.
- `test_wasm.js` - general test jig loading `romwbw.js`; optional ROM file
  argument.
- `test_roms.js` - boots each ROM in `../roms` using the Node build.
- `test_cpm3.js` - Puppeteer regression test for the CP/M 3 boot hang fix.

- `romwbw_node.js` / `romwbw_node.wasm` and `romwbw_test.js` /
  `romwbw_test.wasm` - prebuilt Node-targeted Emscripten builds used by
  the test scripts (`test_roms.js` loads `romwbw_test.js`); the makefile
  has no target to rebuild them.
- `node_modules` - Puppeteer and friends for the Node harnesses.

Sample images:

- `emu_avw.rom`, `emu_romwbw.rom` - 512KB ROM images built for the
  emulator's C++ HBIOS (`emu_avw.rom` is the page's recommended default).
- `hd1k_cpm22.img`, `hd1k_games.img`, `hd1k_infocom.img`,
  `hd1k_zsdos.img` - 8MB single-slice hd1k disk images.

The page's dropdowns also offer `hd1k_combo.img` and `z80cpm_tools.img`,
which live only in the deployment directory, not here.

R8/W8 host file transfer availability on the web-served disks:
`hd1k_combo.img` (slice 0), `hd1k_cpm22.img`, `hd1k_games.img`, and
`z80cpm_tools.img` carry `r8.com` and `w8.com`; `hd1k_infocom.img` and
`hd1k_zsdos.img` do not.

Note that most files listed in this inventory (debug harnesses, Node test
scripts, prebuilt node builds, sample disk images) exist only in the
author's working tree or the deployment directory; a fresh clone contains
only this README, the makefile, `romwbw_web.cc`, the HTML templates, and
the sample ROMs.
