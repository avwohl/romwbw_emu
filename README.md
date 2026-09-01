# RomWBW Emulator

[![GitHub Release](https://img.shields.io/github/v/release/avwohl/romwbw_emu)](https://github.com/avwohl/romwbw_emu/releases/latest)
[![Build Status](https://img.shields.io/github/actions/workflow/status/avwohl/romwbw_emu/release.yml)](https://github.com/avwohl/romwbw_emu/actions/workflows/release.yml)

A hardware-level Z80 emulator for running RomWBW and CP/M from ROM and disk images. Features full Z80 CPU emulation with 512KB ROM + 512KB RAM bank switching and HBIOS hardware abstraction.

## What's New

`romwbw_emu --version` prints the version of the build you have;
[CHANGELOG.md](CHANGELOG.md) is the full record, entry by entry, with the commit
behind each one. Package users come to this release from v1.35: the v1.36 tag
exists as the core-ABI reference point the downstream ports coordinate against,
but no `.deb` or `.rpm` was ever built from it, so two windows of work arrive
together. What changes for a user:

- **The control keys the tty driver used to eat now reach CP/M, and Enter is
  CR.** The CLI's raw mode now owns IXON, IEXTEN and ICRNL, so `^S`/`^Q`, `^V`
  and `^O` go to the guest instead of to the tty driver. The one key still held
  back is the escape character (`^E` by default, `--escape` to change or
  disable), and only on an interactive terminal — see
  [Keyboard](#keyboard). Two consequences: there is no terminal-level
  scroll-pause on `^S` any more, and a pty harness (`expect`, `socat`, `ttyd`)
  must send `\r` for Enter, not `\n`. Piped stdin is unaffected.
- **A `--boot` on the command line no longer rewrites the persisted boot
  target.** It used to be saved at exit like any other NVRAM state, so a test or
  a script silently replaced whatever you had configured. It applies to that run
  only now; a target the guest sets with `SYSCONF` during the run is still
  saved, and `--boot=none` is the way to clear one.
- **`W8` no longer truncates a binary export at the first `1Ah`.** `1Ah` is
  `LD A,(DE)` and occurs in almost any `.COM` - exporting `W8.COM` itself
  produced 368 bytes of 1408 and reported `Done`. Only the run of `1Ah` at the
  very end, which is CP/M's last-record padding, is dropped now.
- **`R8` no longer erases unrelated CP/M files.** It copied the host basename
  into the FCB unfiltered, and a `?` or `*` makes that FCB ambiguous, so the
  `F_DELETE` before `F_MAKE` deleted every file matching it. Host names the CCP
  cannot address afterwards (`_`, a second `.`) are fixed in the same change.
- **`W8` refuses to hand a host path to an emulator that cannot promise it is
  safe** - see [File Transfer (R8/W8)](#file-transfer-r8w8) for the interlock
  and, just as important, what it does not cover.
- **Downstream ports gain two link-time obligations.** A port that syncs these
  sources must define `emu_host_path_caps()` and `emu_host_file_get_read_name()`
  or it will not link. That is deliberate - the core must not assert a safety
  promise on a front end's behalf - and [DOWNSTREAM.md](DOWNSTREAM.md) has the
  answers to give.

## Quick Start

```bash
# Build the emulator
cd src && make

# Run RomWBW (boots to ROM disk)
./romwbw_emu --romwbw=../roms/emu_avw.rom

# Run with a hard disk image
./romwbw_emu --romwbw=../roms/emu_avw.rom --disk0=../disks/hd1k_combo.img
```

At the RomWBW boot menu, type `2` and Enter to boot from the first hard disk, or `C` and Enter for CP/M from ROM. The menu is line-oriented - every command is read as a line, so a bare keystroke only echoes and nothing happens until you press Enter. Boot units 0 and 1 are the on-board RAM and ROM memory disks and carry no operating system, so `0` reports `No system image on disk`; the first `--disk0` image is unit 2 (additional disks are units 3, 4, ...). `D` lists the disk units, `L` the ROM applications, `H` the whole command set, and `W` runs SYSCONF, where a choice can be saved as the autoboot default.

## Installation

### Debian/Ubuntu

```bash
# Download and install the latest .deb package
curl -LO https://github.com/avwohl/romwbw_emu/releases/latest/download/romwbw-emu_amd64.deb
sudo dpkg -i romwbw-emu_amd64.deb

# Run with included ROM
romwbw_emu --romwbw=/usr/share/romwbw_emu/roms/emu_avw.rom
```

For ARM64 systems, use `romwbw-emu_arm64.deb` instead.

### Fedora/RHEL

```bash
# Download and install the latest .rpm package
curl -LO https://github.com/avwohl/romwbw_emu/releases/latest/download/romwbw-emu.x86_64.rpm
sudo rpm -i romwbw-emu.x86_64.rpm

# Run with included ROM
romwbw_emu --romwbw=/usr/share/romwbw_emu/roms/emu_avw.rom
```

For ARM64 systems, use `romwbw-emu.aarch64.rpm` instead.

### What the Packages Install

Both packages stage the same tree (`.github/workflows/release.yml`):

```
/usr/bin/romwbw_emu
/usr/share/romwbw_emu/roms/		every ROM in roms/
/usr/share/romwbw_emu/web/		romwbw.html, romwbw.js, romwbw.wasm, emu_avw.rom
/usr/share/romwbw_emu/web/vendor/	xterm.css, xterm.js, xterm-addon-fit.js, both LICENSEs
/usr/share/doc/romwbw_emu/		README.md, LICENSE
```

The page under `web/` is the one described in
[WebAssembly Version](#webassembly-version), and it works with no internet:
xterm is vendored under `web/vendor/` instead of being pulled from a CDN, and
`emu_avw.rom` is staged a second time next to the page because the page fetches
it by a bare relative name. Before that pair of fixes an installed package
404ed on the only ROM its select offers, always, and opened a page with no
terminal in it whenever the machine had no internet — the template pulled
xterm from a CDN then.

**No disk image is packaged.** The installed page's two disk selects offer five
names - `hd1k_combo.img`, `z80cpm_tools.img`, `hd1k_games.img`,
`hd1k_cpm22.img`, `hd1k_zsdos.img` - and every one of them 404s, including the
two the page selects by default, `hd1k_combo.img` and `hd1k_games.img`.
(`z80cpm_tools.img` is not in this repository at all.) Load an image with
the page's file picker instead. Whether the packages should carry an image
at all is an open question in [`DECISIONS.md`](DECISIONS.md), not an
oversight; the CLI is unaffected, since it takes a path.

### From Source

See [Building](#building) below.

## Disk Images

The emulator supports RomWBW hard disk images in both **hd1k** (modern) and **hd512** (classic) formats. Format is auto-detected from the MBR partition table.

### Recommended Disk Images

```
Image			Size	Description
hd1k_combo.img		49MB	Multi-slice combo disk with CP/M 2.2 and utilities
hd1k_games.img		8MB	Classic games: Colossal Cave, Castle, Dungeon
hd1k_infocom.img	8MB	Infocom text adventures: Zork 1-3, Hitchhiker's Guide
hd1k_cpm22.img		8MB	CP/M 2.2 system disk
hd1k_zsdos.img		8MB	ZSDOS system disk
```

**Before you hand one of these to a mobile port:** the two images this repository ships - `hd1k_combo.img` and `hd1k_infocom.img` - carry `w8.com`, and `W8 <cpmfile> <hostpath>` passes that host path to the front end verbatim. So does any image from elsewhere that was built with a host-path `W8`. An `ioscpm` build before 52 must not be given one. That build stored the path unsanitised as the export filename and then removed the destination first, so `W8 ANYFILE.TXT ..` deleted the app's whole `Documents` folder - every disk image the user had downloaded, imported or created. The `w8.com` in this repository's images refuses a host path unless the emulator answers the `HBF_HOST_CAPS` probe (`disks/verify_disk_utils.sh` asserts the probe is there), which turns that case into a refusal rather than a deletion - but it is a guard inside `W8`, not a boundary: a CP/M program that calls the HBIOS host-file function directly skips it entirely. Update the port, then use the image. The full ordering across the ports is in [docs/RELEASE_ORDER_2026-08-25.md](docs/RELEASE_ORDER_2026-08-25.md).

Of these, `hd1k_combo.img` and `hd1k_infocom.img` are included in this repository under `disks/`; the others must be obtained from the RomWBW release. If downloading from RomWBW directly, use the [RomWBW v3.5.1 release](https://github.com/wwarthen/RomWBW/releases/tag/v3.5.1) Package.zip specifically: the bundled ROM and the emulator's built-in HBIOS identify as v3.5.1, and disk images from a different RomWBW release contain boot slices with a mismatched CBIOS (booting them prints a HBIOS/CBIOS version-mismatch warning). Using a newer release's disks for data files only, without booting from them, is fine.

### RomWBW Version Pin

v3.5.1 is not a passing detail - it is a pin, declared once in
[`src/romwbw_pin.h`](src/romwbw_pin.h) and used to derive the HBIOS version
this emulator reports, the HCB stamped into `src/emu_hbios.asm`, and the ROM
that `roms/build_from_source.sh` builds against. `romwbw_emu --version`
prints it, and a ROM built for a different release is now rejected at load
time with a message naming both versions instead of starting a CPU that
never produces output.

To check that a ROM and disk-image set match the pin - worth doing before
shipping a build, or when a downloaded image misbehaves:

```bash
./roms/verify_romwbw_pin.sh
```

It checks every ROM in `roms/`, every image in `disks/`, and the built
binary, and exits non-zero listing anything that disagrees.

### Disk Format Detection

- **hd1k format**: Detected by partition type 0x2E in MBR, or 8MB file size
  - 1MB prefix, 8MB slices, 16KB system area, 1024 directory entries
- **hd512 format**: Default for other disk images
  - No prefix, 8.3MB slices, 128KB system area, 512 directory entries

### SIMH Compatibility

SIMH AltairZ80 hard disk images (`.dsk` files) are compatible:

```
File Size			Format			Works?
8,388,608 bytes (8 MB)		SIMH HDSK / hd1k	Yes
8,519,680 bytes (8.32 MB)	SIMH HDCPM / hd512	Yes
51,380,224 bytes (49 MB)	RomWBW combo (native)	Yes
```

See `docs/DISK_FORMATS.md` for details.

### Reading an Image with cpmtools

`disks/diskdefs` is the definitive format file for the images here: it carries
`wbw_hd1k` plus `wbw_hd1k_0` through `wbw_hd1k_5`, all six checked against
`hd1k_combo.img`. It exists because a stock cpmtools need not have them - Debian
and Ubuntu's cpmtools 2.23 ships `wbw_hd1k` and nothing per-slice, which is what
once made the disk scripts report "not on the image" for a file that was on it.

```bash
cd disks
cpmls -f wbw_hd1k   hd1k_infocom.img   # plain 8 MB image
cpmls -f wbw_hd1k_0 hd1k_combo.img     # combo: slice 0, past the 1 MB MBR
```

Run cpmtools from `disks/` so that it picks up the local file: cpmtools reads
`./diskdefs` OR the system copy, never both, and a `DISKDEFS` variable pointing
at a path that does not exist is ignored silently. **The wrong diskdef does not
fail** - it prints a garbage directory, which reads as "no such file". One
build-specific limit: the libdsk backend a Debian or Ubuntu cpmtools is built
with cannot address anything past 8 MB from the start of the image, so
`wbw_hd1k_1` and up answer "cannot read superblock (Bad parameter)" there. That
is the build, not the definitions - the header comment in `disks/diskdefs` says
so, and a `device_posix` build of the same sources reads all six.

### Drive Letters

Drive letters are not fixed. The CBIOS builds the map at cold boot out of what
HBIOS reports, so it depends on how many hard disks are attached and on what you
booted (`docs/drive_assignment.md`):

- Slices per hard disk = `max(2, 8 / number of hard disks)`. One disk gets 8
  slices, two get 4 each, three or more get 2 each.
- On a **ROM boot** the memory disks come first: `A:` is the RAM disk (MD0),
  `B:` the ROM disk (MD1), and the hard-disk slices follow.
- On a **disk boot** the booted slice becomes `A:` and the memory disks follow
  it. The boot loader records the slice in the HCB, and CBIOS reads it back.

Both maps below are what the guest printed under `Configuring Drives...`.

ROM boot, two disks - `--disk0=hd1k_combo.img --disk1=hd1k_infocom.img --boot=C`,
so four slices each:

```
A:=MD0:0	B:=MD1:0	C:=HDSK0:0	D:=HDSK0:1	E:=HDSK0:2
F:=HDSK0:3	G:=HDSK1:0	H:=HDSK1:1	I:=HDSK1:2	J:=HDSK1:3
```

Disk boot, one disk - `--disk0=hd1k_combo.img --boot=2`, so eight slices, and
the slice that was booted is `A:`:

```
A:=HDSK0:0	B:=MD0:0	C:=MD1:0	D:=HDSK0:1	E:=HDSK0:2
F:=HDSK0:3	G:=HDSK0:4	H:=HDSK0:5	I:=HDSK0:6	J:=HDSK0:7
```

CP/M's own `ASSIGN` shows the live map and changes it (`ASSIGN D:=HDSK0:2`),
which is also how to reach a slice the automatic map did not cover.

## File Transfer (R8/W8)

The `R8` and `W8` CP/M utilities (sources: `src/r8.asm`, `src/w8.asm`) copy files between the host and CP/M. They talk to the emulator through HBIOS extension functions 0xE1-0xEA.

**CLI:** `R8 <hostpath>` imports a host file into CP/M. The path is used as typed - relative paths resolve against the directory `romwbw_emu` was started from, and absolute paths work. CP/M's CCP uppercases the command line, so the emulator retries host paths case-insensitively (typing `R8 /home/me/file.txt` works even though CP/M delivers `/HOME/ME/FILE.TXT`). The CP/M-side name is the uppercased 8.3 basename.

`W8 <cpmfile> [hostpath]` exports a CP/M file to the host. With no `hostpath` it lands in the emulator's working directory under the CP/M name, lowercased - what it always did. With one, it goes there instead. Both utilities take **the whole rest of the command line** as the path, so a directory whose name contains a space works: `W8 OUT.TXT /Users/me/My Documents/out.txt`. Trailing spaces are trimmed.

Before it sends a host path, `W8` asks the emulator whether host paths are
handled safely: `HBF_HOST_CAPS` (0xE9), a probe with no inputs and no state, so
it can be asked before anything has been opened. An emulator that predates the
call answers "no such function", and one that has the call but does not set
`CAP_SAFE_PATH` answers no. Either way `W8` prints `This emulator is too old to
be given a host path safely.` / `Nothing was written.` and stops without
opening, creating or truncating anything. **Only the path form is withheld** -
`W8 FOO.TXT` with no path still works on any emulator, because that name comes
from the FCB and the CCP cannot put a `.` in an FCB name field. This is a guard
inside `W8`, not a trust boundary: a `.COM` that calls the HBIOS host-file
function directly skips the probe entirely. `disks/verify_disk_utils.sh` asserts
the probe bytes are present in the `w8.com` on each tracked image.

Because the CCP uppercases the whole line, the emulator resolves the *directory* components case-insensitively and lowercases the final name. The typed case cannot be recovered - CP/M destroys it before the emulator sees it - so lowercase is a convention, not a guess at what you meant. That means **the path you type is not the path that gets written**, which is why `W8` does not echo it: it asks the emulator where the file actually went and prints that.

`R8` does the same with the file it read. The two are usually the same file - it has to exist for the open to succeed - but not the same string, so `Reading:` names the file that was really opened, in the case it really has, as an absolute path. On a front end whose read is a file picker there is no answer to give and `R8` prints what you typed, as before.

When either utility cannot open the host file it prints the message and then `  Asked for: <path>` on the next line. The path is labelled because it is the request: nothing was created, and it is in the case the CCP shouted rather than the case the emulator would have used.

```
C>W8 MYFILE.TXT /home/me/out.txt
W8 - Write to host filesystem
Writing: MYFILE.TXT
To host: /home/me/out.txt
Done: 4096 bytes
```

`W8` copies the file whole. It drops only the run of `^Z` characters at the very end, which is the padding CP/M writes into the last record of a file - so a text file imported with `R8` comes back byte for byte, and a `.COM` containing `1Ah` bytes (`LD A,(DE)`, which is common) is no longer truncated at the first one. The one thing that cannot survive is a file whose real content *ends* in `1Ah`: CP/M stores no length, only whole 128-byte records, so nothing can tell those bytes from the padding and they are dropped with it. Write errors at close (e.g. host disk full) are reported: `Host file close failed - file may be truncated`, and a failure on the CP/M side is now told apart from end of file: `Error: CP/M read failed - the host file is short`.

**Web:** `R8` opens a browser file picker (the emulator pauses until you pick a file or cancel); `W8` triggers a browser download. A browser has no filesystem to honour a directory with, so a `hostpath` given there is reduced to its last component and becomes the suggested download name - and `To host:` says so, rather than repeating a path that means nothing in a browser. The same is true of the sandboxed mobile ports, where the file goes to the app's own export location.

```
C>R8 /home/me/getkey.com
C>W8 MYFILE.TXT
```

The first command imports the host file `getkey.com` as `GETKEY.COM` on the current drive; the second exports `MYFILE.TXT` as `myfile.txt` in the directory the emulator was started from.

**Availability:** `r8.com` and `w8.com` are on `disks/hd1k_combo.img` (slice 0) and `disks/hd1k_infocom.img`. Images published elsewhere - a GitHub release asset, a downstream port's bundled copy - carry whatever `r8.com`/`w8.com` they were built with, and are not updated by a change here. `disks/verify_disk_utils.sh` checks the tracked images against the sources and `disks/rebuild_disk_utils.sh` refreshes them; `make -C src test` runs the first of those.

## WebAssembly Version

Try RomWBW in your browser - no installation required:

```bash
cd web
make                                            # builds romwbw.js and romwbw.wasm
sed "s/@VERSION@/$(cat ../VERSION)/" romwbw.html-template > romwbw.html
make serve                                      # http://localhost:8080/romwbw.html
```

Two things the first line needs: emscripten, and a sibling `cpmemu` checkout -
`web/makefile` hardcodes `QKZ80_SRC = ../../cpmemu/src` for the Z80 core.

The `sed` step is not optional. The page itself is not checked in - the
template is - and no makefile target renders it, so `make` leaves the wasm with
no `romwbw.html` to load it and `make serve` serves exactly that. The release
workflow performs this substitution before packaging, and `make deploy-dev`
does it on the way to `~/www/romwbw1/index.html`. (The tracked
`romwbw-debug.html` is a separate page, for the `make romwbw-debug.js` build.)

Load your own ROM and disk images through the web interface.

In the browser, `R8` imports files via a file picker and `W8` exports them as downloads - see [File Transfer (R8/W8)](#file-transfer-r8w8).

The web UI remembers your control selections (ROM choice, disk selections, boot string, and the "don't warn" checkboxes) in browser localStorage, so they survive page reloads; clearing the browser's site data resets them to defaults. The Debug checkbox is deliberately not persisted, and local file uploads cannot be restored by the browser, so those revert to defaults on reload.

## Building

```bash
cd src/
make           # build romwbw_emu
make test      # build and run the test suites
```

**Requirements:** a C++11 compiler, a POSIX system for the CLI front end, and
**qkz80**, the Z80 core from the [cpmemu](https://github.com/avwohl/cpmemu)
project. CI builds and tests the whole tree with gcc on Ubuntu and Apple clang
on macOS. The Windows job is narrower on purpose: it compiles the three files
`z80cpmw` pulls out of `src/` (`emu_init.cc`, `hbios_cpu.cc`,
`hbios_dispatch.cc`) with `cl /c` under that project's own `/W3 /std:c++17`, so
nothing links and no test runs there - it is a check that the shared core still
compiles with MSVC, not that this tree builds on Windows.

qkz80 is the dependency a fresh clone is missing. `src/makefile` resolves it
four ways, in this order: `QKZ80_CFLAGS`/`QKZ80_LIBS` set by you (put them in
`src/local.mk`), a `pkg-config qkz80` entry, a sibling checkout at `../cpmemu`
with `make libqkz80.a` run in it, and finally `/usr/local`. That last one is a
last resort, not a default: it links whatever Z80 core happens to be installed
there, which need not be the one this tree is developed against, and the build
prints five warnings saying so (`src/makefile:45-49`).

```bash
make qkz80-source   # says which of the four this tree would use, without building
```

Other targets: `make test` runs the four C++ suites, the two node JS suites when
node is on `PATH` (skipped, not failed, when it is not), and
`disks/verify_disk_utils.sh`; `make install` and `make uninstall` honour
`PREFIX` (default `/usr/local`) and `DESTDIR`; `make STATIC=1` links statically,
which is what the release workflow builds.

For WebAssembly see [WebAssembly Version](#webassembly-version) above; that
build needs emscripten and the same sibling cpmemu checkout.

## Features

- **Memory:** 512KB ROM + 512KB RAM with 32KB bank switching
- **HBIOS:** Hardware abstraction layer implemented in C++
- **Disks:** ROM disk, RAM disk, and file-backed hard disk images
- **Disk Formats:** Auto-detects hd1k and hd512 RomWBW formats
- **Console:** Raw mode, so every control character reaches the guest; the VT emulation is the host terminal's job (xterm.js in the browser)
- **File Transfer:** R8/W8 utilities copy files between the host and CP/M (CLI paths or browser picker/download)
- **Settings file:** A JSON machine description in place of a long command line
- **Debugger:** A `sim>` prompt with breakpoints, single-step, register and memory dumps and a symbol table (there is no disassembler - `dm` prints bytes)
- **RomWBW pin:** Built and checked against RomWBW v3.5.1 (`roms/verify_romwbw_pin.sh`)
- **WebAssembly:** Run RomWBW in any modern browser

## Boot Configuration

The emulator supports automatic boot configuration via NVRAM. Settings persist across sessions in `$XDG_CONFIG_HOME/romwbw_emu/nvram` (default `~/.config/romwbw_emu/nvram`), a plain text file containing a single line such as `C` or `2.3`.

### Quick Boot Examples

```bash
# Auto-boot to CP/M (ROM app 'C')
./romwbw_emu --romwbw=roms/emu_avw.rom --boot=C

# Auto-boot from the first hard disk (unit 2), slice 0
./romwbw_emu --romwbw=roms/emu_avw.rom --disk0=disk.img --boot=2

# Auto-boot unit 2 (first hard disk), slice 3
./romwbw_emu --romwbw=roms/emu_avw.rom --disk0=d0.img --disk1=d1.img --disk2=d2.img --boot=2.3

# Show boot menu (default)
./romwbw_emu --romwbw=roms/emu_avw.rom --boot=H
```

### Using SYSCONF (Interactive Configuration)

Type `W` and Enter at the boot menu to reach the RomWBW SYSCONF utility:

```
Boot [H=Help]: W

RomWBW System Config Utility, Version 1.0

Commands:
  P           - Print current settings
  S BO D,u,s  - Set boot to Disk unit u, slice s
  S BO R,app  - Set boot to ROM app (C=CP/M, Z=ZSDOS, etc.)
  S AB E,t    - Enable autoboot with t second timeout (0=immediate)
  S AB D      - Disable autoboot
  R           - Reset NVRAM to defaults
  Q           - Quit

Examples:
  S BO D,2,3  - Boot from disk 2, slice 3
  S BO R,C    - Boot CP/M from ROM
  S AB E,5    - Enable autoboot with 5 second countdown
  S AB E,0    - Enable autoboot immediately (no countdown)
```

Settings configured via SYSCONF are saved automatically when the emulator exits.

### Boot Format Reference

```
--boot=C	Boot ROM app C (CP/M 2.2)
--boot=Z	Boot ROM app Z (the ROM menu calls it Z-System; ZSDOS elsewhere here)
--boot=B	Boot ROM app B (BASIC) - any ROM app letter works, not just C and Z
--boot=2	Boot the first hard disk (unit 2), slice 0
--boot=2.3	Boot unit 2 (the first hard disk), slice 3
--boot=H	Show the boot menu
--boot=none	Forget the persisted boot target and show the menu (off is the same)
```

`--boot` takes what you would have typed at the menu, and `L` there lists ten
ROM applications, not two: `M` Monitor, `C` CP/M 2.2, `Z` Z-System, `B` BASIC,
`T` Tasty BASIC, `F` Forth, `P` Play a Game, `N` Network Boot, `X` XModem Flash
Updater, `U` User App. The menu itself also takes `D` (device inventory), `R`
(reboot), `W` (SYSCONF), `I <u> [<c>]` (console interface and baud rate) and
`V [<n>]` (HBIOS diagnostic verbosity) - each of them a line, terminated by
Enter.

A `--boot` given on the command line applies to **that run only** and is not
written back, so a script cannot change what you boot by default. A boot target
the guest sets with `SYSCONF` during the run still is. `--boot=none` is the way
to undo one.

Boot unit numbers: 0 = RAM disk, 1 = ROM disk, 2 and up = hard disks in `--disk0`, `--disk1`, ... order. Type `D` and Enter at the boot menu to list them.

## Command Line Options

```
./romwbw_emu --romwbw=<rom.rom> [options]
./romwbw_emu <rom.rom> [options]      # a bare path is taken as the ROM

Options:
  --version, -v     Print the version and the pinned RomWBW release, then exit
  --help, -h        Print the built-in usage, then exit
  --romwbw=FILE     Enable RomWBW mode with ROM file
  --boot=CMD        Auto-boot command (C, Z, 2, 2.3, H, etc.), this run only
  --boot=none       Forget the persisted boot target ('off' is the same)
  --debug           Enable debug output
  --strict-io       Halt on unexpected I/O ports

Disk options:
  --diskN=FILE      Attach a disk image to hard-disk slot N, for N = 0..15.
                    --disk0 is boot unit 2, --disk1 unit 3, and so on; which
                    drive letters they get depends on how many disks are
                    attached - see Drive Letters above.

ROM contents:
  --romapp=K=Name:path  Add a ROM application under boot menu key K
  --romapp=K:path       Same, with the name derived from the key
  --romldr=FILE     Use FILE as the RomWBW boot loader (romldr) image

Other options:
  --escape=CHAR     Key reserved for console mode (default ^E)
  --escape=none     Reserve no key; every byte reaches CP/M

Settings file:
  --config=FILE     Load settings from a JSON file (this beats --no-config)
  --no-config       Ignore auto-discovered settings files
  --save-config[=F] Write the effective settings as JSON and exit
                    (F defaults to ./romwbw_emu.json)

NVRAM persistence:
  NVRAM is persisted to $XDG_CONFIG_HOME/romwbw_emu/nvram (default ~/.config/romwbw_emu/nvram)
  Use SYSCONF (W at the boot menu) to configure interactively.
```

`--version`, `-v`, `--help` and `-h` are answered before any settings file is
read, so they still work when a malformed `romwbw_emu.json` is sitting in the
current directory.

### Debugging and Low-Level Options

These are for looking inside a run rather than for using the emulator. Apart
from `--symbols`, which has a `symbols` key in the settings file as well, they
are command-line only by design (`src/emu_config.h`): a per-run diagnostic is
not something a config file should be able to turn on behind you.

```
  --symbols=FILE    Load a symbol table (.sym), so sim> takes .LABEL where it
                    takes an address (bp .BDOS, e .BDOS) and annotates the
                    addresses it prints back
  --trace=FILE      Write an execution trace to FILE
  --load=ADDR       Load address written into that trace script (with --trace)
  --start=ADDR      Begin execution at ADDR instead of 0x0000
  --sense=N         Set the sense switches to N (0x... accepted)
  --mask-interrupt <min>-<max> <rst|call> <n>
                    Fire a maskable interrupt somewhere in that cycle range.
                    Three separate arguments, not one =value: a range (or a
                    single cycle count), then rst or call, then the RST number
                    or the call address.
  --nmi <min>-<max> Fire an NMI (vector 0x0066) in that cycle range
```

## Keyboard

Every control character goes to the guest, because CP/M software uses them:
`^R` retypes the current line at the CCP prompt, `^E`/`^S`/`^D`/`^X` are the
WordStar cursor diamond, `^Q` starts the WordStar `^Qx` commands, `^O` starts
the `^Ox` onscreen-format commands, and `^C` warm-boots. The emulator claims no
Ctrl-letter for itself, with one exception.

**On an interactive terminal the escape character is reserved by the emulator
and never reaches CP/M.**
`--escape=CHAR` names the key that suspends the guest and drops you at the
`sim>` prompt, where `help` lists the debugger commands and `quit` exits. The
default is `^E`, which is WordStar cursor-up, so if you run WordStar, VDE or
anything else built on that layout, either move the key (`--escape=^]`) or turn
it off entirely with `--escape=none`. `CHAR` is `^A` through `^_`, a literal
character, or `none` (`off` and `^@` mean the same); with `none` there is no
way into `sim>` at all and every control character reaches the guest. The same
value can live in a settings file as `"escape": "none"` — see
[docs/CONFIGURATION.md](docs/CONFIGURATION.md).

That reservation is a property of a terminal, not of the emulator: with piped
or redirected stdin nothing is reserved at all, and a script's own `0x05`
reaches the guest instead of dropping the run into `sim>`. The startup banner
says which case you are in - `reserved by the emulator, the guest never sees it`
on a tty, `reserved on an interactive terminal only; stdin is not a tty, so the
key reaches the guest` on a pipe.

The terminal is put in raw mode with XON/XOFF flow control (`^S`/`^Q`) and the
line discipline's literal-next and discard keys (`^V`/`^O`) disabled, so those
four reach the guest instead of the tty driver. One consequence worth knowing:
there is no terminal-level scroll-pause on `^S` any more, because `^S` is now
the guest's key.

Enter arrives as CR and `Ctrl+J` sends LF — they are distinct keys. If you
drive the emulator from a pty harness (`expect`, `socat`, `ttyd`), send `\r`
for Enter, not `\n`. Piped and redirected stdin is unaffected: a script's
LF-terminated lines still work, because the terminal settings above apply only
to a real tty.

In the browser, xterm.js translates `Ctrl`+letter to the control byte and the
page forwards it verbatim, and the terminal takes focus on load — so `Ctrl+R`
is CP/M's retype-line, not a page reload. `Ctrl+Shift`+letter sends the same
byte as `Ctrl`+letter, except for the combinations the browser owns
(`Ctrl+Shift+V` paste, and the devtools and tab/window shortcuts). `Ctrl+W`,
`Ctrl+T`, `Ctrl+N` and `Ctrl+Q` still reach the browser as well as the guest —
no page can prevent that — so the tab warns before closing while the emulator
is running or a disk has unsaved writes.

## Settings File

The machine description (ROM, disks, boot command, escape char, ROM apps)
can live in a JSON settings file instead of a long command line — an idea
imported from the z80cpmw Windows port. Save your current command line with
`--save-config`, then run `romwbw_emu` bare:

```bash
./romwbw_emu --romwbw=roms/emu_avw.rom --disk0=disks/hd1k_combo.img --boot=2 --save-config
./romwbw_emu     # boots the saved machine
```

The emulator looks for `./romwbw_emu.json`, then
`$XDG_CONFIG_HOME/romwbw_emu/config.json` (default
`~/.config/romwbw_emu/config.json`); `--config=FILE` names a file explicitly
and `--no-config` disables discovery. CLI flags always override file values,
and a loaded file is announced with a `[CONFIG]` banner. See
[docs/CONFIGURATION.md](docs/CONFIGURATION.md) for the schema.

The keys the loader reads are `version`, `rom`, `boot`, `escape`, `symbols`,
`romldr`, `debug`, `strictIo`, `disks[]` (index = unit, at most 16) and
`romapps[]` (`{key,name,path}` objects). **A key it does not know is ignored
without a word**: a file with `romm` in place of `rom` loads cleanly, prints its
`[CONFIG]` banner, and then the run fails with `Error: No binary file
specified`, a message that points at the command line rather than at the typo.
`--save-config` is the way to see the schema the loader actually reads.

Two smaller details. `XDG_CONFIG_HOME` is honoured only when it is absolute; a
relative value falls back to `~/.config` silently. And `--config=FILE` beats
`--no-config` no matter what order they appear in - `--no-config` suppresses
discovery, not an explicitly named file.

## Examples

```bash
# Boot from ROM disk (default)
./romwbw_emu --romwbw=roms/emu_avw.rom

# Boot with hard disk attached
./romwbw_emu --romwbw=roms/emu_avw.rom --disk0=disks/hd1k_combo.img

# Boot with two disks attached (this is the ROM-boot map under Drive Letters)
./romwbw_emu --romwbw=roms/emu_avw.rom --disk0=disks/hd1k_combo.img --disk1=disks/hd1k_infocom.img
```

## Project Structure

```
romwbw_emu/
  src/
    romwbw_emu.cc		CLI main: argument parsing, NVRAM, and the sim> debugger
    hbios_dispatch.*		HBIOS service handlers - the bulk of the emulation
    hbios_cpu.*			CPU subclass: the port I/O that reaches HBIOS
    romwbw_mem.h		Bank-switched memory (512KB ROM + 512KB RAM)
    emu_io.h			The interface every front end implements
    emu_io_cli.cc		Terminal and host-file back end for the CLI
    emu_io_wasm.cc		The same for the browser
    emu_init.*			Start-up shared by all front ends (ROM load, HCB, disks)
    emu_config.*		JSON settings file
    romwbw_pin.h		The RomWBW version pin
    version.cc			Version and build stamp
    r8.asm w8.asm		The CP/M file-transfer utilities
    emu_hbios.asm		The Z80 HBIOS stub inside the emulator ROM
  tests/			4 C++ suites and 2 node JS suites (make -C src test)
  web/
    romwbw.html-template	The page; romwbw.html is rendered from it
    romwbw_web.cc		WebAssembly front end
    vendor/			Vendored xterm, so the page needs no CDN
  roms/				ROM images and build scripts
  disks/			Disk images, diskdefs, and the R8/W8 install scripts
  docs/				Technical documentation
  archive/			Retired material kept for reference
  CHANGELOG.md DECISIONS.md DOWNSTREAM.md MANUAL_CHECKS.md todo.txt VERSION
```

## Documentation

- [CHANGELOG.md](CHANGELOG.md) - What changed in each release, with the commit behind every entry
- [DOWNSTREAM.md](DOWNSTREAM.md) - What a port must do to take a sync of this core; `docs/DOWNSTREAM_*.md` are the dated notices, newest first
- [MANUAL_CHECKS.md](MANUAL_CHECKS.md) - The checks that need a person at a keyboard, which no test here can settle
- `todo.txt` - Open work, every item tagged with what a machine has to be able to do to take it
- [DECISIONS.md](DECISIONS.md) - The open questions that need the owner's ruling rather than a machine; split out of `todo.txt`, where no capability tag could describe them
- `docs/BOOT_CONFIGURATION.md` - Boot options, SYSCONF utility, NVRAM persistence
- [docs/CONFIGURATION.md](docs/CONFIGURATION.md) - The JSON settings file and its schema
- `docs/DISK_FORMATS.md` - Disk formats, SIMH compatibility, and cpmtools usage
- `docs/disk-images.md` - Working with the disk images by hand
- `docs/drive_assignment.md` - How CBIOS builds the drive map, and what the emulator has to report for it to work
- `docs/ARCHITECTURE.md` - Emulator architecture and the shared C++ HBIOS implementation
- `docs/HBIOS_Implementation_Guide.md` - How HBIOS is implemented
- `docs/HBIOS_DATA_EXPORTS.md` - HBIOS data structures
- `docs/ROM_ATTESTATION.md` - Where the bundled ROM images come from and under what rights
- [docs/RELEASE_ORDER_2026-08-25.md](docs/RELEASE_ORDER_2026-08-25.md) - The order the ports have to release in, for the `W8` host-path work

## License

GNU General Public License v3.0 - see [LICENSE](LICENSE).

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

