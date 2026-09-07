# RomWBW WebAssembly Frontend

Browser frontend for the RomWBW emulator. The shared C++ engine is compiled
to WebAssembly with Emscripten and driven from a single-page terminal UI
built on xterm.js 5.3.0 (plus the fit addon 0.8.0). Both are vendored in
`vendor/` and loaded from there by `romwbw.html-template`; 5920681 took the
jsdelivr tags out, because release.yml staged the page, the wasm and the ROM
and no xterm, so an installed .deb on a machine with no internet opened a page
with no terminal in it. The page now talks to no third-party host at all.
`vendor/README.md` records which npm tarballs the bytes came from, and why the
tags no longer carry an `integrity=` (SRI is a check on a file fetched from a
host you do not control; a same-origin file shipped inside the package is the
file the hash would be over).

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
- `make clean` - remove `romwbw.js` and `romwbw.wasm`, plus the debug build's
  `.js`, `.wasm` and `.wasm.map`. No `.data` is removed because none is
  produced: `romwbw-bundled.js` was the only `--preload-file` target and it is
  gone (see below).
- `make mirror-dev` / `make mirror-prod` - run `../tools/romwbw-get mirror`
  into `~/www/romwbw1` / `~/www/romwbw`, writing the `catalog/` directory the
  page reads its ROM and disk lists from. `MIRROR_FLAGS` defaults to
  `--versions=runnable --emu=../src/romwbw_emu`, so only releases the CLI
  binary says it can boot are mirrored - it and the wasm are compiled from the
  same `../src/romwbw_pin.h`, so it is the right thing to ask. Both releases
  with every disk is roughly 460 MB; `MIRROR_ONLY=--only=emu_avw,hd1k_combo`
  trims a small host.
- `make serve` - mirror `emu_avw` and `hd1k_combo` into this directory, then
  serve it with `python3 -m http.server 8080`. The mirror step is what makes a
  local serve boot at all: this repository tracks no ROM and no disk image, and
  before v1.40 the page offered five filenames that no build or packaging step
  ever put next to it.
- `make deploy-dev` - depends on `mirror-dev`; deploys to `~/www/romwbw1`.
  Safe to run without asking; the version string is stamped with `-dev` and a
  timestamp.
- `make deploy-romwbw-PRODUCTION-ASK-HUMAN-FIRST` - depends on `mirror-prod`;
  deploys to the production directory `~/www/romwbw`. **Do not run this without
  explicit human approval - the target name is the policy.**

`make romwbw-bundled.js` is gone. It preloaded `../roms/emu_avw.rom` into the
Emscripten filesystem as `/romwbw.rom` via `--preload-file` for the
`_romwbw_autostart` export to find, and both the ROM and that export were
deleted in v1.40. The target's prerequisite no longer exists, and baking a
512 KB ROM into a `.data` file is exactly what made publishing a new ROM a
release of this repository.

Versioning: the makefile reads `../VERSION` and passes it as
`-DEMU_VERSION` at compile time. Both deploy targets generate `index.html`
by sed-substituting every `@VERSION@` in `romwbw.html-template` with that
version (the template also uses it as a `romwbw.js?v=...` cache-buster).
The deploy targets copy `index.html`, `romwbw.js`, `romwbw.wasm` and the five
files of `vendor/` (a deploy that leaves those out is a page with no terminal
in it); the ROM and disk images the page offers come from the `catalog/`
mirror, which those same targets now write themselves - see "The catalog
mirror" below.

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
  the page triggers a browser download. `filename` is always a bare name,
  never a path: W8 may be given a host path, and the core reduces it with
  `emu_host_path_basename()` (both separators) and lowercases it, because a
  name with a separator in it is not a usable download filename. The blob can
  be zero bytes - an empty CP/M file is a real file and still downloads.
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
  (ROM choice, disk 0/1 selections, boot string, and the per-disk "don't
  warn" checkboxes) persist in browser `localStorage`. That is six values and
  no slice setting: the two slice `<select>`s were deleted in 2dbf6f2 because
  they fed `Module._romwbw_set_disk_slices`, which does not exist.
  Local file uploads cannot be restored (browsers forbid programmatic
  file-input values) and the Debug checkbox is deliberately session-only.
- A `beforeunload` handler warns before the tab closes if any disk unit
  has guest writes that have not been downloaded, using
  `_romwbw_is_disk_dirty`. Downloading a disk clears its dirty flag, and
  dirty state survives Stop.

## The catalog mirror

The page carries no ROM and no disk image, and it cannot fetch one from GitHub.
Measured: a romwbw_disks release download URL sends no
`access-control-allow-origin` on either redirect hop and the CORS preflight
OPTIONS returns 404. `raw.githubusercontent.com` and jsdelivr do send `*` and
carry a byte-identical copy of the catalog *documents*, but the ROMs and images
are release assets only - `git ls-files | grep -cE '\.(img|rom)$'` in
romwbw_disks is 0 - so those hosts 404 on them, and there is no Pages site. A
browser physically cannot reach the artifacts.

So the page reads everything from its own origin, and
`../tools/romwbw-get mirror <dir>` is what puts it there:

    <dir>/catalog/manifest.json          what the page reads at load time
    <dir>/catalog/v0/<release>/<file>    the ROMs and images themselves

`manifest.json` is a `romwbw-emu-mirror` document. Per RomWBW release it has a
label, the catalog's `generation`, and a `roms` and a `disks` array; each entry
keeps the catalog's `id`, `name`, `description`, `filename`, `size`, `sha256`,
`format`, `default` / `defaultSlot`, `host_transfer` and a `url` relative to
`catalog/`. Anything left out of the mirror is listed in `not_mirrored` with
the reason, so a partial mirror says what is missing instead of offering a
shorter list that looks complete. `emu_supported` is the release list the CLI
binary reported at mirror time; a release outside it is shown disabled and
labelled "(needs a newer build)" rather than hidden, so a user who mirrored one
can see why it is not selectable.

What the page does with it:

- `loadManifest()` fills the RomWBW, ROM, Disk 0 and Disk 1 selects at load
  time; no `<option>` in the markup names an artifact. The six it does carry
  are placeholders - `-- no catalog mirror --`, `-- Select ROM --`, and
  `-- None --` / `-- Custom --` on each disk select - and they are load
  bearing: they are what leaves both file pickers usable on a page with no
  mirror behind it. Everything keys on the manifest's `id`, never on a filename
  or an array position, and fields it does not know are ignored - the
  conformance rules romwbw_disks publishes for a catalog consumer, which this
  is one hop removed from.
- Changing the RomWBW select rebuilds both the ROM and the disk lists, because
  a ROM from one release with a disk from another boots into
  `*** WARNING: HBIOS/CBIOS Version Mismatch ***`.
- Every download is checked against the `size` and `sha256` its `<option>`
  carries before the emulator is handed a byte of it (`verifyAsset`). A file
  the user picked from their own computer carries neither and is not checked.
- `crypto.subtle` exists only in a secure context - https, or
  `http://localhost`. A `make serve` reached over a LAN by IP has none, which
  is exactly the case where silently not checking would be worst, so it *says*
  it checked the size only rather than reporting a pass it did not perform.
- A directory with no `catalog/manifest.json` is not an error. The status line
  and the terminal say there is no mirror and name the command that makes one,
  and both file pickers keep working. An installed `.deb` is in that state
  until someone runs `romwbw-get mirror /usr/share/romwbw_emu/web`. Before
  v1.40 an installed page instead 404'd on the only ROM its select offered and
  on both of its default disks.

Both deploy targets depend on the matching mirror target, so **a newly
published ROM or disk reaches the page by re-running a deploy** - no source
edit, no wasm rebuild, no release of this repository. See
[../docs/CATALOG.md](../docs/CATALOG.md).

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
- `vendor/` - xterm.js 5.3.0, the fit addon 0.8.0, their two MIT licences,
  and a README recording the npm tarballs they were taken from. The page
  loads the terminal from here, so both deploy targets and release.yml's
  staging step copy this directory; unlike most of what follows, it is
  checked in.

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
- `test_roms.js` - boots each ROM in `../roms` using the Node build. `../roms`
  holds no ROM since v1.40, so this now has nothing to iterate over even once
  the Node build below is buildable again; point it at a
  `romwbw-get` cache or a mirror.
- `test_cpm3.js` - Puppeteer regression test for the CP/M 3 boot hang fix.
- `node_modules` - Puppeteer and friends for the Node harnesses.

The prebuilt Node-targeted Emscripten builds these scripts loaded -
`romwbw_node.js` / `romwbw_node.wasm` and `romwbw_test.js` /
`romwbw_test.wasm` - are **gone**, deleted 2026-09-01. They were December 2025
builds, they were never tracked (gitignored, like every script in this list),
and the makefile has no target to rebuild them, so they had drifted five months
behind the sources with no way to tell and no way to refresh. `test_roms.js`
loaded `romwbw_test.js` and therefore cannot run until someone writes a target
that builds a Node-targeted wasm.

Note that none of the scripts above is in the repository either. The three web
tests that ARE tracked and that `make -C src test` runs are
`tests/web_console_output.js`, `tests/web_reload_disks.js` and
`tests/web_manifest.js`, and all three lift functions out of
`romwbw.html-template` by text extraction rather than loading any wasm, so they
are unaffected - and a rename or a reindent in the template fails loudly in
them.

The mirror:

- `catalog/manifest.json` and `catalog/v0/<release>/` - written by
  `make serve`, `make mirror-dev` or `make mirror-prod`, gitignored, and the
  only source of the page's ROM and disk lists.

There is no longer a list of sample images here to go stale, and that is the
point of the mirror. `emu_avw.rom` and `emu_romwbw.rom` were tracked in this
directory until v1.40 as copies of two files in `../roms/`; the four
`hd1k_*.img` names this section used to list beside them were in no directory
of this repository at all, and neither was `z80cpm_tools.img`. Which disks
exist, which is the recommended default, and which carry `r8.com` / `w8.com`
are questions the catalog answers - `romwbw-get list` prints them, the
manifest's `host_transfer` flag carries the last one, and the page builds its
own "R8.COM and W8.COM are on ..." sentence from that rather than from a
sentence somebody has to remember to edit.

So most of what this inventory names (the debug harnesses, the Node test
scripts, the prebuilt node builds, the mirror) exists only in a working tree
or a deployment directory. A fresh clone of this directory contains this
README, the makefile, `romwbw_web.cc`, the HTML templates and `vendor/`.
