# Building, Packages and Layout

How to build the emulator and the browser version, what the .deb and .rpm
install, and where things live in the tree.

## What the packages install

`romwbw_emu`, `romwbw-get`, and the batch tools `romwbw-batch` and
`romwbw-plm80` ([BATCH.md](BATCH.md), [ISX.md](ISX.md))
into `/usr/bin`; the copy of cpmemu's `cpm_disk.py` the batch tools edit disk
images with into `/usr/share/romwbw_emu/` - not onto PATH, where cpmemu's own
package puts `cpm_disk`; `README.md`, the licence, `USAGE.md`, `ROMS_AND_DISKS.md`, `CATALOG.md`,
`BATCH.md`, `ISX.md` and `isxbios.asm`, the source of the BIOS `romwbw-batch` carries,
into `/usr/share/doc/romwbw-emu/`; and the browser version under
`/usr/share/romwbw_emu/web/` with xterm vendored beside it, so it opens with no
internet. The emulator itself needs no Python; the three tools do, and the
packages recommend `python3`, which `apt install ./romwbw-emu_amd64.deb` and
`dnf install ./romwbw-emu.x86_64.rpm` pull in and `dpkg -i` and `rpm -i` do
not. Neither package contains a ROM or a disk image, and the installed page
has neither until you populate the mirror beside it:

```bash
sudo romwbw-get mirror /usr/share/romwbw_emu/web
```

Until then the page says so, and its file pickers still load an image you
already have.

## From source

See [Building](#building). Note that `make install` installs the emulator
binary only - `romwbw-get`, `romwbw-batch` and `romwbw-plm80` are staged by
the release workflow, so from a source build run them out of `tools/`, where
the batch tools find `cpm_disk.py` in a cpmemu checkout beside this one.

## Building

```bash
cd src/
make           # build romwbw_emu
make test      # build and run the test suites
```

### Requirements

- a C++11 compiler and a POSIX system for the CLI front end
- **um80** and **ul80** (`pip install um80`) for `make test`, which assembles
  `src/r8.asm` and `src/w8.asm`. Without them `disks/verify_disk_utils.sh`
  skips its source gate and still exits 0, so the suite goes green having
  checked less than it looks like
- **qkz80**, the Z80 core from [cpmemu](https://github.com/avwohl/cpmemu),
  normally a sibling checkout at `../cpmemu` with `make -C ../cpmemu/src
  libqkz80.a` run once
- emscripten, for the WebAssembly build only

qkz80 is the dependency a fresh clone is missing. `src/makefile` resolves it
four ways, in order: `QKZ80_CFLAGS`/`QKZ80_LIBS` set by you (put them in
`src/local.mk`), a `pkg-config qkz80` entry, the sibling checkout at
`../cpmemu/src` (which is where its makefile and `libqkz80.a` live), and finally
`/usr/local` - a last resort that links whatever core happens to be installed
there, and the build prints warnings saying so.

```bash
make qkz80-source   # says which of the four this tree would use, without building
```

`make test` runs the C++, node and Python suites and both shell verifiers, and
needs no network; a suite whose interpreter is missing is skipped, not failed.
`make install` and `make uninstall` honour `PREFIX` (default `/usr/local`) and
`DESTDIR`; `make STATIC=1` links statically, which is what the release workflow
builds.

## WebAssembly Version

```bash
cd web
make serve     # builds the wasm, renders the page, http://localhost:8080/romwbw.html
```

This needs emscripten and a sibling `cpmemu` checkout - `web/makefile`
hardcodes `QKZ80_SRC = ../../cpmemu/src` for the Z80 core. `romwbw.html` is
rendered from `romwbw.html-template`, not tracked; `make serve` stamps it
`<VERSION>-local`, and the release workflow renders it with the bare `VERSION`.
The two deploy targets write `~/www/.../index.html` rather than `romwbw.html`:
`make deploy-dev` stamps `<VERSION>-dev` with a timestamp, and
`deploy-romwbw-PRODUCTION-ASK-HUMAN-FIRST` uses the bare `VERSION`. `make serve` also mirrors `emu_avw` and `hd1k_combo` into
`web/` first, so a local serve comes up with a ROM and a bootable disk in its
selects - which needs the network the first time and nothing after that.

The page builds its RomWBW, ROM and disk lists at load time from
`catalog/manifest.json` beside it, written by `romwbw-get mirror`, and checks
each download against the size and SHA-256 the manifest carries. A page with no
mirror says so and still loads your own images through its file pickers. The UI
remembers your control selections in localStorage; the Debug checkbox and any
file you uploaded yourself are not restored on reload.

## Project Structure

```
romwbw_emu/
  src/
    makefile			The build; local.mk overrides it
    romwbw_emu.cc		CLI main: argument parsing, NVRAM, and the sim> debugger
    hbios_dispatch.*		HBIOS service handlers - the bulk of the emulation
    hbios_cpu.*			CPU subclass: the port I/O that reaches HBIOS
    romwbw_mem.h		Bank-switched memory (512KB ROM + 512KB RAM)
    emu_io*			The front-end interface, its shared half, and the
				CLI and browser back ends
    emu_init.*			Start-up shared by all front ends (ROM load, HCB, disks)
    emu_config.*		JSON settings file (vendored nlohmann in include/)
    r8.asm w8.asm		The CP/M file-transfer utilities
    emu_hbios.asm emu_rom.asm	The Z80 side of the emulator ROM
  tests/			10 C++, 5 node JS and 2 Python suites (make -C src test)
  web/				makefile, romwbw.html-template (romwbw.html is
				rendered from it), romwbw_web.cc, and vendor/
				for xterm, so the page needs no CDN
  tools/
    romwbw-get			Fetches and verifies the ROM and the disk images
    romwbw-batch		Runs CP/M commands unattended and extracts the results
    romwbw-plm80		Intel PL/M-80 under DRI's ISX, by DRI's recipes
    isxbios.asm			The compact BIOS romwbw-batch runs ISX on
    unreleased.sh		What is finished here and not in anyone's hands
  roms/				build_emu_rom.sh and verify_romwbw_pin.sh - no ROM is tracked
  disks/			verify_disk_utils.sh - no image is tracked
  docs/				Technical documentation
  .github/workflows/		test.yml and release.yml
  archive/			Retired material kept for reference
  CHANGELOG.md DECISIONS.md DOWNSTREAM.md MANUAL_CHECKS.md todo.txt VERSION
```
