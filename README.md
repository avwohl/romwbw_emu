# RomWBW Emulator

[![GitHub Release](https://img.shields.io/github/v/release/avwohl/romwbw_emu)](https://github.com/avwohl/romwbw_emu/releases/latest)
[![Tests](https://img.shields.io/github/actions/workflow/status/avwohl/romwbw_emu/test.yml?label=tests)](https://github.com/avwohl/romwbw_emu/actions/workflows/test.yml)

A hardware-level Z80 emulator that boots RomWBW and CP/M from ROM and disk
images, on Linux, macOS and in the browser. It emulates the machine - 512 KB
ROM + 512 KB RAM with bank switching - and implements the
[RomWBW](https://github.com/wwarthen/RomWBW) HBIOS layer in C++.

**This program ships no ROM and no disk image.** Both are published at
[avwohl/romwbw_disks](https://github.com/avwohl/romwbw_disks) and fetched by
`tools/romwbw-get`, which checks every download against the SHA-256 the catalog
publishes - see [docs/CATALOG.md](docs/CATALOG.md).

What changed in each version: [CHANGELOG.md](CHANGELOG.md).

## Features

- **Memory:** 512 KB ROM + 512 KB RAM with 32 KB bank switching
- **HBIOS:** hardware abstraction layer implemented in C++
- **Disks:** ROM disk, RAM disk, and up to 16 file-backed hard disk images
- **Disk formats:** auto-detects hd1k and hd512, including 49 MB combo images
- **RomWBW releases:** boots 3.5.1 and 3.6.0 from one binary, reading the
  version out of the ROM it loads
- **Operating systems:** CP/M 2.2, CP/M 3, ZSDOS, ZPM3, NZCOM and QPM, from the
  published disk images
- **Console:** raw mode, so every control character reaches the guest; the VT
  emulation is the host terminal's job (xterm.js in the browser)
- **File transfer:** `R8`/`W8` copy files between CP/M and the host
- **Settings file:** a JSON machine description in place of a long command line
- **Debugger:** a `sim>` prompt with breakpoints, single-step, register and
  memory dumps, and a symbol table - CLI only, and with no disassembler; `dm`
  prints bytes
- **WebAssembly:** runs in any modern browser

## Quick Start

```bash
git clone https://github.com/avwohl/cpmemu ../cpmemu   # the Z80 core
make -C ../cpmemu/src libqkz80.a
make -C src                                            # build the emulator
tools/romwbw-get run                                   # fetch a ROM and a disk, verify them, and boot
```

The first two lines are the one dependency a fresh clone is missing; see
[Building](#building) for the other three ways to supply it. Anything after a
bare `--` goes to the emulator: `tools/romwbw-get run -- --boot=2.1`.

To drive the emulator yourself, ask for the paths:

```bash
src/romwbw_emu --romwbw="$(tools/romwbw-get path @rom)" \
               --disk0="$(tools/romwbw-get path --work @disk0)"
```

`@rom` and `@disk0` are the catalog's defaults for the selected release; any
id from `romwbw-get list` works in their place. `path` prints the hash-verified
download, which is read-only, and `path --work` prints a writable copy - which
is what a disk needs, because the guest writes to it. `romwbw-get verify`
re-hashes the cache against the catalog and `--repair` re-fetches what does not
match; `--offline` before a subcommand never opens a socket, and `cache-dir`
prints the cache root.

### Boot menu keys

Every command is read as a line, so nothing happens until you press Enter.

- `2` - boot the first hard disk (`--disk0`), slice 0; `2.3` for slice 3
- `C` - boot CP/M 2.2 from ROM; `Z` for Z-System
- `D` - list the boot units
- `W` - SYSCONF, RomWBW's configuration utility
- `H` - the command set

Units 0 and 1 are the on-board RAM and ROM memory disks and carry no operating
system. `--disk0` is unit 2, `--disk1` unit 3, and so on.

**`H` shows a different menu on each release.** On 3.6.0 it lists the ten ROM
applications alongside the commands. On 3.5.1 it lists seven commands and no
application letters - `C` and `Z` work, but you need `L` (List ROM
Applications) to see them, and `L` is a 3.5.1 command that 3.6.0 answers with
`*** Invalid command`.

## Installation

### Debian/Ubuntu

```bash
curl -LO https://github.com/avwohl/romwbw_emu/releases/latest/download/romwbw-emu_amd64.deb
sudo dpkg -i romwbw-emu_amd64.deb
romwbw-get run
```

Use `romwbw-emu_arm64.deb` on ARM64.

### Fedora/RHEL

```bash
curl -LO https://github.com/avwohl/romwbw_emu/releases/latest/download/romwbw-emu.x86_64.rpm
sudo rpm -i romwbw-emu.x86_64.rpm
romwbw-get run
```

Use `romwbw-emu.aarch64.rpm` on ARM64.

### macOS

No package; build from source, which is what CI tests with Apple clang.

### What the packages install

`romwbw_emu` and `romwbw-get` into `/usr/bin`, and the browser version under
`/usr/share/romwbw_emu/web/` with xterm vendored beside it, so it opens with no
internet. Neither package contains a ROM or a disk image, and the installed page
has neither until you populate the mirror beside it:

```bash
sudo romwbw-get mirror /usr/share/romwbw_emu/web
```

Until then the page says so, and its file pickers still load an image you
already have.

### From source

See [Building](#building). Note that `make install` installs the emulator
binary only - `romwbw-get` is staged by the release workflow, so from a source
build run it out of `tools/`.

## ROMs and Disk Images

`romwbw-get list` prints the ROMs and disks a release publishes - a couple of
dozen per release: system disks, language toolchains and games. Fetch one by
its id:

```bash
tools/romwbw-get fetch hd1k_games
src/romwbw_emu --romwbw="$(tools/romwbw-get path @rom)" \
               --disk0="$(tools/romwbw-get path --work hd1k_games)"
```

`--romwbw=` wants an `emu_*` ROM, not a stock RomWBW one. Bank 0 of an `emu_*`
ROM is this project's HBIOS proxy; a ROM built for real hardware drives
peripherals that are not here, and the emulator warns and carries on rather
than refusing.

`hd1k_combo` is the catalog's slot-0 disk, so it is what `@disk0` and a bare
`romwbw-get run` resolve to: 49 MB, six slices, CP/M 2.2 and the utilities. It
is also the only published image carrying `R8` and `W8`, which `list` marks
`R8/W8`.

### RomWBW releases

One binary boots any release named in `ROMWBW_SUPPORTED_RELEASES`
([`src/romwbw_pin.h`](src/romwbw_pin.h)) - today 3.5.1 and 3.6.0 - and
`romwbw_emu --version` prints the list. The version the guest sees is read from
the loaded ROM's HBIOS Configuration Block at run time, not compiled in. A
release this core has not been checked against is refused, because bank 0 of an
`emu_*.rom` is this project's C++ dispatcher and a CBIOS calling something it
does not implement would load and then hang; `--allow-untested-romwbw`
overrides that with a warning.

`romwbw-get use 3.5.1` picks a release and remembers it; `--romwbw VER` before
a subcommand does it for one run.

**A ROM and the disk you boot have to be the same release**, or the guest
prints `*** WARNING: HBIOS/CBIOS Version Mismatch ***`. `romwbw-get` only ever
hands out a matched pair. Using another release's disks for data files, without
booting from them, is fine. `./roms/verify_romwbw_pin.sh [tree]` checks every
ROM and image it can find, in the tree you name and in the `romwbw-get` cache,
against `src/romwbw_pin.h`; `make -C src test` runs it.

### Disk formats

Format is auto-detected:

| | |
|---|---|
| **hd1k, single slice** | exactly 8,388,608 bytes, no prefix. 16 KB system area, 1024 directory entries |
| **hd1k combo** | partition type 0x2E in the MBR. A 1 MB prefix, then 8 MB slices laid out as plain hd1k images |
| **hd512** | the fallback. No prefix, 8.32 MB slices (8,519,680 bytes), 128 KB system area, 512 directory entries |

SIMH AltairZ80 hard disk images work directly at either of the two single-disk
sizes above - 8,388,608 and 8,519,680 bytes. See
[docs/DISK_FORMATS.md](docs/DISK_FORMATS.md).

### Reading an image

Use `cpm_disk.py`, a stdlib-only Python file owned by
[cpmemu](https://github.com/avwohl/cpmemu) at `util/cpm_disk.py` and kept in no
copy here. Point `$CPM_DISK` at it, keep a cpmemu checkout beside this one, or
run cpmemu's `make install`.

```bash
CPM=~/src/cpmemu/util/cpm_disk.py
COMBO=$(tools/romwbw-get path @disk0)              # 49 MB combo, mode 0444

python3 "$CPM" list "$COMBO"                       # slice 0, auto-detected
python3 "$CPM" list --slice 3 "$COMBO"             # any of the six slices
mkdir -p out && python3 "$CPM" extract "$COMBO" W8.COM -o out

WORK=$(tools/romwbw-get path --work @disk0)        # a writable copy
python3 "$CPM" delete --slice 0 "$WORK" W8.COM
python3 "$CPM" add    --slice 0 "$WORK" ./w8.com
```

The format is taken from the file size, so there is nothing to configure and
no definitions file to pick.

### Drive letters

Drive letters are not fixed: CBIOS builds the map at cold boot from what HBIOS
reports, so it depends on how many hard disks are attached and on what you
booted. Slices per hard disk = `max(2, 8 / number of hard disks)`.

On a **ROM boot** the memory disks come first - `hd1k_combo` in `--disk0`,
`hd1k_infocom` in `--disk1`, `--boot=C`:

```
A:=MD0:0	B:=MD1:0	C:=HDSK0:0	D:=HDSK0:1	E:=HDSK0:2
F:=HDSK0:3	G:=HDSK1:0	H:=HDSK1:1	I:=HDSK1:2	J:=HDSK1:3
```

On a **disk boot** the booted slice becomes `A:` - `hd1k_combo` in `--disk0`,
`--boot=2`:

```
A:=HDSK0:0	B:=MD0:0	C:=MD1:0	D:=HDSK0:1	E:=HDSK0:2
F:=HDSK0:3	G:=HDSK0:4	H:=HDSK0:5	I:=HDSK0:6	J:=HDSK0:7
```

CP/M's `ASSIGN` shows the live map and changes it (`ASSIGN D:=HDSK0:2`), which
is how to reach a slice the automatic map did not cover. See
[docs/drive_assignment.md](docs/drive_assignment.md).

## File Transfer (R8/W8)

`R8` imports a host file into CP/M; `W8` exports a CP/M file to the host.

```
C>R8 /home/me/getkey.com          imports the host file as GETKEY.COM
C>W8 MYFILE.TXT                   exports it as ./myfile.txt on the host
C>W8 MYFILE.TXT /home/me/out.txt  exports it to that path instead
```

The host path is the whole rest of the command line, so a directory name may
contain spaces. With no host path `W8` writes the CP/M name, lowercased, into
the directory the emulator was started from. In the browser `R8` opens a file
picker and `W8` downloads.

Both live on slice 0 of `hd1k_combo`, so **which drive they are on depends on
how you booted**: after a ROM boot (`--boot=C`) that slice is `C:` and you land
on `B:`, so type `C:` first or CP/M answers `R8?`. After a disk boot
(`--boot=2`) it is `A:` and they are already on your path.

[docs/FILE_TRANSFER.md](docs/FILE_TRANSFER.md) has the rest: the naming and
case rules, the host-path interlock, what survives a round trip, every message
the two print, and which images carry them.

## Boot Configuration

`--boot` takes what you would have typed at the boot menu:

```
--boot=C	Boot ROM app C (CP/M 2.2); any ROM app letter works
--boot=Z	Boot ROM app Z (Z-System)
--boot=2	Boot the first hard disk (unit 2), slice 0
--boot=2.3	Boot unit 2, slice 3
--boot=H	Show the boot menu
--boot=none	Forget the persisted boot target and show the menu ('off' is the same)
```

The ROM applications are `M` Monitor, `C` CP/M 2.2, `Z` Z-System, `B` BASIC,
`T` Tasty BASIC, `F` Forth, `P` Play a Game, `N` Network Boot, `X` XModem Flash
Updater and `U` User App.

A `--boot` on the command line applies to **that run only** and is not written
back, so a script cannot change what you boot by default. A target the guest
sets with `SYSCONF` during the run is saved; `--boot=none` is the way to undo
one. NVRAM lives in `$XDG_CONFIG_HOME/romwbw_emu/nvram` (default
`~/.config/romwbw_emu/nvram`), a single line such as `C` or `2.3`.

Type `W` at the boot menu for SYSCONF, where a boot choice can be saved as the
autoboot default. Its command set and worked examples are in
[docs/BOOT_CONFIGURATION.md](docs/BOOT_CONFIGURATION.md).

## Command Line Options

`romwbw_emu --help` prints the built-in usage. It covers most of these, but not
`--romapp`, `--romldr`, or `--sense`, `--load`, `--start`, `--mask-interrupt`
and `--nmi` below; `--trace` and `--symbols` it does list. The ones in daily
use:

```
./romwbw_emu --romwbw=<rom.rom> [options]
./romwbw_emu <rom.rom> [options]      # a bare path is taken as the ROM

  --version, -v     Print the version and the RomWBW releases this build runs
  --help, -h        Print the built-in usage
  --romwbw=FILE     The ROM image
  --diskN=FILE      Attach a disk image to hard-disk slot N, for N = 0..15.
                    --disk0 is boot unit 2, --disk1 unit 3, and so on.
  --boot=CMD        Auto-boot command (C, Z, 2, 2.3, H, none), this run only
  --escape=CHAR     Key reserved for the sim> prompt (default ^E)
  --escape=none     Reserve no key; every byte reaches CP/M
  --romapp=K=Name:path  Add a ROM application under boot menu key K
  --romapp=K:path       Same, with the name derived from the key
  --romldr=FILE     Use FILE as the RomWBW boot loader (romldr) image
  --config=FILE     Load settings from a JSON file (this beats --no-config)
  --no-config       Ignore auto-discovered settings files
  --save-config[=F] Write the effective settings as JSON and exit
  --debug           Enable debug output
  --strict-io       Halt on unexpected I/O ports
  --allow-untested-romwbw
                    Load a ROM from a RomWBW release this build has not been
                    checked against
```

`--version`, `-v`, `--help` and `-h` are answered before any settings file is
read, so they work even with a malformed `romwbw_emu.json` in the directory.

### Debugging options

`--symbols=FILE` loads a symbol table so `sim>` takes `.LABEL` where it takes
an address, and annotates the addresses it prints. `--trace=FILE` writes an
execution trace, with `--load=ADDR` naming the load address written into it.
`--start=ADDR` begins execution somewhere other than `0x0000`, and `--sense=N`
sets the sense switches (`0x...` accepted). `--mask-interrupt <min>-<max> <rst|call> <n>` and
`--nmi <min>-<max>` fire an interrupt in a cycle range - three separate
arguments, not one `=value`.

These are command-line only: a per-run diagnostic is not something a settings
file should turn on behind you. `--symbols` is the exception, and has a
`symbols` key too.

## Keyboard

Every control character goes to the guest, because CP/M software uses them:
`^R` retypes the line at the CCP prompt, `^E`/`^S`/`^D`/`^X` are the WordStar
cursor diamond, `^Q` and `^O` prefix its command sets, and `^C` warm-boots. The
terminal is put in raw mode with IXON and the line discipline's literal-next
and discard keys cleared, so `^S`, `^Q`, `^V` and `^O` reach the guest rather
than the tty driver - which also means there is no terminal-level scroll-pause
on `^S`. IXOFF is deliberately left alone: it only governs the kernel throttling
a fast sender and never consumes a typed `^S`.

**On an interactive terminal the escape character is reserved and never reaches
CP/M.** It suspends the guest and drops you at the `sim>` prompt, where `help`
lists the debugger commands and `quit` exits. The default `^E` is WordStar
cursor-up, so under WordStar or VDE move it (`--escape=^]`) or turn it off
(`--escape=none`); `CHAR` is `^A` through `^_`, a literal character, or `none` (`off` means the same).
With piped stdin nothing is reserved at all, and the startup banner says which
case you are in.

Enter arrives as CR and `Ctrl+J` sends LF - they are distinct keys. A pty
harness (`expect`, `socat`, `ttyd`) must send `\r` for Enter, not `\n`. Piped
stdin is unaffected.

In the browser, xterm.js translates `Ctrl`+letter to the control byte and the
page forwards it verbatim, and the terminal takes focus on load, so `Ctrl+R` is
CP/M's retype-line rather than a page reload. `Ctrl+W`, `Ctrl+T`, `Ctrl+N` and
`Ctrl+Q` still reach the browser as well - no page can prevent that - so the
tab warns before closing while the emulator is running or a disk has unsaved
writes.

## Settings File

The machine description (ROM, disks, boot command, escape character, ROM apps)
can live in a JSON file instead of a long command line. Save your current
command line, then run bare:

```bash
src/romwbw_emu --romwbw="$(tools/romwbw-get path @rom)" \
               --disk0="$(tools/romwbw-get path --work @disk0)" \
               --boot=2 --save-config
src/romwbw_emu   # boots the saved machine
```

`--save-config` writes the paths as they were resolved, so the saved file names
the cached ROM and the working copy directly and never consults the catalog
again. It writes to `--save-config=FILE` if given, else to the `--config=` file
if there was one, else to `./romwbw_emu.json` - never to a discovered XDG path.

The emulator looks for `./romwbw_emu.json`, then
`$XDG_CONFIG_HOME/romwbw_emu/config.json`; `--config=FILE` names one explicitly
and `--no-config` disables discovery. CLI flags always override file values.

A key the loader does not recognise is ignored without a word, so
`--save-config` is the way to see the schema it actually reads. The keys are
`version`, `rom`, `boot`, `escape`, `symbols`, `romldr`, `debug`, `strictIo`,
`disks[]` and `romapps[]`; [docs/CONFIGURATION.md](docs/CONFIGURATION.md) has
the schema. `romwbw-get` keeps its own release and artifact choice in a
separate file, `$XDG_CONFIG_HOME/romwbw_emu/catalog.json`.

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
    romwbw_pin.h		The RomWBW releases this core can run
    r8.asm w8.asm		The CP/M file-transfer utilities
    emu_hbios.asm emu_rom.asm	The Z80 side of the emulator ROM
  tests/			4 C++, 3 node JS and 1 Python suite (make -C src test)
  web/				makefile, romwbw.html-template (romwbw.html is
				rendered from it), romwbw_web.cc, and vendor/
				for xterm, so the page needs no CDN
  tools/
    romwbw-get			Fetches and verifies the ROM and the disk images
    unreleased.sh		What is finished here and not in anyone's hands
  roms/				build_emu_rom.sh and verify_romwbw_pin.sh - no ROM is tracked
  disks/			verify_disk_utils.sh - no image is tracked
  docs/				Technical documentation
  .github/workflows/		test.yml and release.yml
  archive/			Retired material kept for reference
  CHANGELOG.md DECISIONS.md DOWNSTREAM.md MANUAL_CHECKS.md todo.txt VERSION
```

## Documentation

Process files: [CHANGELOG.md](CHANGELOG.md) (what changed in each release, with
the commit behind every entry), [DOWNSTREAM.md](DOWNSTREAM.md) (what a port must
do to take a sync of this core; `docs/DOWNSTREAM_*.md` are the dated notices),
[MANUAL_CHECKS.md](MANUAL_CHECKS.md), [DECISIONS.md](DECISIONS.md) and
`todo.txt`.

- [docs/CATALOG.md](docs/CATALOG.md) - where the ROM and the disk images come from, and how `romwbw-get` verifies them
- [docs/BOOT_CONFIGURATION.md](docs/BOOT_CONFIGURATION.md) - boot options, the boot menu, SYSCONF, NVRAM
- [docs/CONFIGURATION.md](docs/CONFIGURATION.md) - the JSON settings file and its schema
- [docs/FILE_TRANSFER.md](docs/FILE_TRANSFER.md) - R8/W8 in full
- [docs/DISK_FORMATS.md](docs/DISK_FORMATS.md) - disk formats, SIMH compatibility, and reading images with `cpm_disk.py`
- [docs/disk-images.md](docs/disk-images.md) - working with disk images by hand
- [docs/drive_assignment.md](docs/drive_assignment.md) - how CBIOS builds the drive map
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - emulator architecture and the shared C++ HBIOS
- [docs/HBIOS_Implementation_Guide.md](docs/HBIOS_Implementation_Guide.md) - how HBIOS is implemented
- [docs/HBIOS_DATA_EXPORTS.md](docs/HBIOS_DATA_EXPORTS.md) - HBIOS data structures
- [docs/ROM_ATTESTATION.md](docs/ROM_ATTESTATION.md) - the rights the `emu_avw` ROM is distributed under
- [docs/IOS-notes.md](docs/IOS-notes.md) - notes from the iOS/macOS port
- [web/README.md](web/README.md) - the browser front end's own notes

## License

GNU General Public License v3.0 - see [LICENSE](LICENSE).

### Third-Party Licenses

- **RomWBW** is GPLv3. Banks 1-15 of an `emu_*.rom` are upstream RomWBW; bank 0
  is this project's. See [docs/ROM_ATTESTATION.md](docs/ROM_ATTESTATION.md).
- **xterm.js**, vendored under `web/vendor/`, ships its own LICENSE beside it.
- **The disk images** carry CP/M and third-party CP/M software under their own
  terms; this repository distributes none of them.

## Related Projects

- [80un](https://github.com/avwohl/80un) - Unpacker for the CP/M archive and compression formats LBR, ARC, squeeze, crunch, and CrLZH.
- [cpmdroid](https://github.com/avwohl/cpmdroid) - Z80/CP/M emulator for Android phones and tablets. It emulates the RomWBW HBIOS interface and a VT100 terminal.
- [cpmemu](https://github.com/avwohl/cpmemu) - Z80/CP/M emulator for Linux and Windows, with Z80 and 8080 CPU cores. It translates the BDOS and BIOS calls of CP/M 2.2 programs to the host file system.
- [ioscpm](https://github.com/avwohl/ioscpm) - Z80/CP/M emulator for iOS and macOS. It emulates the RomWBW HBIOS interface and runs CP/M 2.2 and CP/M 3.
- [learn-ada-z80](https://github.com/avwohl/learn-ada-z80) - Collection of more than 90 Ada example programs for uada80, the Ada compiler for the Z80 processor and CP/M.
- [mbasic](https://github.com/avwohl/mbasic) - Python interpreter for MBASIC 5.21, the Microsoft BASIC-80 for CP/M. Two compiler backends compile the programs to CP/M .COM files or to JavaScript.
- [mbasic2025](https://github.com/avwohl/mbasic2025) - Reconstruction of the lost source code of MBASIC 5.21, the Microsoft BASIC-80 for CP/M. The MACRO-80 source code assembles to a binary that matches mbasic.com byte for byte.
- [mbasicc](https://github.com/avwohl/mbasicc) - C++17 interpreter for MBASIC 5.21, the Microsoft BASIC-80 for CP/M. It runs on Linux and macOS.
- [mbasicc_web](https://github.com/avwohl/mbasicc_web) - Web browser interpreter for MBASIC 5.21, the Microsoft BASIC-80 for CP/M. Emscripten compiles the mbasicc interpreter to WebAssembly.
- [mpm2](https://github.com/avwohl/mpm2) - Z80 emulator for MP/M II, the multi-user CP/M operating system. Users connect over SSH, and SFTP clients transfer files.
- [scelbal](https://github.com/avwohl/scelbal) - Floating-point BASIC interpreter for the 8080 processor and CP/M. A translator converts the original 8008 source code to 8080 source code.
- [uada80](https://github.com/avwohl/uada80) - Ada compiler for the Z80 processor and CP/M 2.2. It compiles a subset of Ada 2012 to CP/M .COM files.
- [uc80](https://github.com/avwohl/uc80) - C compiler for the Z80 processor and CP/M. It optimizes for small code size.
- [ucow](https://github.com/avwohl/ucow) - Cowgol compiler for the Z80 processor and CP/M. It runs on Linux in Python.
- [um80_and_friends](https://github.com/avwohl/um80_and_friends) - Linux toolchain that is compatible with Microsoft MACRO-80. It has an assembler, a linker, a librarian, and a disassembler.
- [upeepz80](https://github.com/avwohl/upeepz80) - Peephole optimizer for Z80 compilers that write lowercase Z80 assembly language. It shortens jumps to jr, builds djnz loops, and removes dead stores.
- [uplm80](https://github.com/avwohl/uplm80) - PL/M-80 compiler for the Z80 processor and CP/M. It writes Intel 8080 and Zilog Z80 assembly language.
- [z80cpmw](https://github.com/avwohl/z80cpmw) - Z80/CP/M emulator for Windows. It emulates the RomWBW HBIOS interface and boots CP/M from disk images.

## See Also

- [RomWBW](https://github.com/wwarthen/RomWBW) - The original RomWBW project by Wayne Warthen
