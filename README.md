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

- **Since v1.40 no ROM and no disk image ships with this program.** They are
  published at [avwohl/romwbw_disks](https://github.com/avwohl/romwbw_disks) and
  fetched by `tools/romwbw-get`, which checks every download against the SHA-256
  the catalog publishes; `romwbw-get run` fetches a matched ROM and disk and
  boots them. The point of the move is that a disk published there reaches an
  emulator that is already installed, with no release of this repository - see
  [docs/CATALOG.md](docs/CATALOG.md).
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
make -C src

# Fetch a ROM and a disk, verify them, and boot
tools/romwbw-get run
```

That second command is the whole quick start. **This repository ships no ROM and
no disk image.** `tools/romwbw-get` reads the catalog published by
[avwohl/romwbw_disks](https://github.com/avwohl/romwbw_disks), works out which
RomWBW release this build can boot, downloads that release's default ROM and
disk, checks both against the SHA-256 the catalog publishes, and starts the
emulator. Anything after a bare `--` goes to the emulator
(`tools/romwbw-get run -- --boot=2.1`), and
[docs/CATALOG.md](docs/CATALOG.md) is the rest of the story - the shape of the
catalog, what the exit codes mean, and why the artifacts left this repository.

To drive the emulator yourself, ask for the paths:

```bash
src/romwbw_emu --romwbw="$(tools/romwbw-get path @rom)" \
               --disk0="$(tools/romwbw-get path --work @disk0)"
```

`path` prints the hash-verified download, which is read-only; `path --work`
prints a writable copy of a disk. They are deliberately different files: a guest
writes to its disks, so the first CP/M `SAVE` against the cached original would
break the hash of a file the tool has promised is intact.

At the RomWBW boot menu, type `2` and Enter to boot from the first hard disk, or `C` and Enter for CP/M from ROM. The menu is line-oriented - every command is read as a line, so a bare keystroke only echoes and nothing happens until you press Enter. Boot units 0 and 1 are the on-board RAM and ROM memory disks and carry no operating system, so `0` reports `*** No boot record` on RomWBW 3.6.0 and `*** No system image on disk` on 3.5.1; the first `--disk0` image is unit 2 (additional disks are units 3, 4, ...). `D` lists the disk units, `H` the whole command set, and `W` runs SYSCONF, where a choice can be saved as the autoboot default. 3.5.1 lists the ROM applications under `L`; 3.6.0 answers `L` with `*** Invalid command` and folds them into `H`.

## Installation

### Debian/Ubuntu

```bash
# Download and install the latest .deb package
curl -LO https://github.com/avwohl/romwbw_emu/releases/latest/download/romwbw-emu_amd64.deb
sudo dpkg -i romwbw-emu_amd64.deb

# Fetch a ROM and a disk and boot them - no ROM is packaged, see below
romwbw-get run
```

For ARM64 systems, use `romwbw-emu_arm64.deb` instead.

### Fedora/RHEL

```bash
# Download and install the latest .rpm package
curl -LO https://github.com/avwohl/romwbw_emu/releases/latest/download/romwbw-emu.x86_64.rpm
sudo rpm -i romwbw-emu.x86_64.rpm

# Fetch a ROM and a disk and boot them - no ROM is packaged, see below
romwbw-get run
```

For ARM64 systems, use `romwbw-emu.aarch64.rpm` instead.

### What the Packages Install

Both packages stage the same tree (`.github/workflows/release.yml`):

```
/usr/bin/romwbw_emu
/usr/bin/romwbw-get			the catalog client - the only way to get a ROM
/usr/share/romwbw_emu/web/		romwbw.html, romwbw.js, romwbw.wasm
/usr/share/romwbw_emu/web/vendor/	xterm.css, xterm.js, xterm-addon-fit.js, both LICENSEs
/usr/share/doc/romwbw_emu/		README.md, CATALOG.md, LICENSE
```

**Neither package contains a ROM or a disk image.** Up to v1.39 they carried six
512 KB ROMs under `roms/` plus a seventh copy of `emu_avw.rom` beside the page -
about 3.7 MB of a package whose binary is 400 KB - and staging them is what made
"publish a new ROM" mean "release this repository". `romwbw-get` is installed
into `/usr/bin` instead, as a first-class program rather than a developer
script, because the emulator's own error message names it when it is started
with no ROM. A step in `release.yml` asserts that no `.rom` or `.img` reached
the staging tree, since the two `cp` lines that used to put them there were the
kind of thing a later edit quietly restores.

The page under `web/` is the one described in
[WebAssembly Version](#webassembly-version), and it opens with no internet:
xterm is vendored under `web/vendor/` instead of being pulled from a CDN. Before
that fix an installed package opened a page with no terminal in it whenever the
machine had no internet — the template pulled xterm from a CDN then.

**The installed page has no ROM and no disk until somebody populates the mirror
beside it**, which is one command:

```bash
sudo romwbw-get mirror /usr/share/romwbw_emu/web
```

Until then the page says so, in its status line and in its terminal, and both
file pickers work regardless, so an image you already have still boots. That is
the answer to the question [`DECISIONS.md`](DECISIONS.md) carried as section 4 -
whether a package should carry a disk image at all - and the section has left
that file, as an answered one does. What it replaced was worse than carrying
nothing: the installed page 404ed on the only ROM its select offered and on both
of the disks it selected by default, none of which anything ever staged.

### From Source

See [Building](#building) below.

## Disk Images

The emulator supports RomWBW hard disk images in both **hd1k** (modern) and **hd512** (classic) formats. Format is auto-detected from the MBR partition table.

### Where the Images Come From

[avwohl/romwbw_disks](https://github.com/avwohl/romwbw_disks) builds and
publishes a matched set of ROMs and disk images for every supported RomWBW
release, and `tools/romwbw-get` fetches them. None of them is tracked here, so a
disk published there is available to an emulator that is already installed - see
[docs/CATALOG.md](docs/CATALOG.md). `romwbw-get list` prints what a release has:

```
$ tools/romwbw-get list
RomWBW 3.6.0  (generation 2)

ROMs:
  emu_avw         512.0 KB  cached   EMU AVW  [default]
  emu_rcz80       512.0 KB           EMU RCZ80

Disks:
  hd1k_combo       49.0 MB  cached   Combo (Recommended)  [slot 0, R8/W8]
  hd1k_cpm22        8.0 MB           CP/M 2.2
  hd1k_zsdos        8.0 MB           ZSDOS
  hd1k_zpm3         8.0 MB           ZPM3
  hd1k_cpm3         8.0 MB           CP/M 3
  hd1k_nzcom        8.0 MB           NZCOM
  hd1k_qpm          8.0 MB           QPM
  hd1k_games        8.0 MB           Games
  hd1k_infocom      8.0 MB           Infocom Adventures
  [15 more, hd1k_aztecc through hd1k_msxroms2]
```

That is 24 disks in 3.6.0 and 20 in 3.5.1 - system disks, language toolchains
and games: `hd1k_games` is Colossal Cave, Castle and Dungeon, `hd1k_infocom` is
Zork 1-3 and the Hitchhiker's Guide. Fetch one by its id and hand it to the
emulator:

```bash
tools/romwbw-get fetch hd1k_games
src/romwbw_emu --romwbw="$(tools/romwbw-get path @rom)" \
               --disk0="$(tools/romwbw-get path --work hd1k_games)"
```

`hd1k_combo` is the disk the catalog puts in slot 0, so it is what `@disk0` and
a bare `romwbw-get run` resolve to: 49 MB, six slices, CP/M 2.2 and the
utilities. It is also the only published image that carries `R8` and `W8` - the
catalog sets `host_transfer` on it and nothing else, which is what `list` prints
as `R8/W8`.

**Before you hand an image to a mobile port:** the published `hd1k_combo` image carries `w8.com`, and `W8 <cpmfile> <hostpath>` passes that host path to the front end verbatim. So does any image from elsewhere that was built with a host-path `W8`. An `ioscpm` build before 52 must not be given one. That build stored the path unsanitised as the export filename and then removed the destination first, so `W8 ANYFILE.TXT ..` deleted the app's whole `Documents` folder - every disk image the user had downloaded, imported or created. The `w8.com` in the published images refuses a host path unless the emulator answers the `HBF_HOST_CAPS` probe (`disks/verify_disk_utils.sh` asserts the probe is in the `w8.com` it builds from source, and in any image it is pointed at), which turns that case into a refusal rather than a deletion - but it is a guard inside `W8`, not a boundary: a CP/M program that calls the HBIOS host-file function directly skips it entirely. Update the port, then use the image. The ordering the ports were asked to release in is recorded, as history, in [docs/RELEASE_ORDER_2026-08-25.md](docs/RELEASE_ORDER_2026-08-25.md) - its publish steps name scripts and images v1.40 removed, so read it for the sequence and not for the commands.

**A ROM and the disk you boot have to be the same release.** `romwbw-get` only
ever hands out a matched pair, because it resolves both inside one release's
catalog; a pair assembled by hand from two releases loads - the emulator runs
either - and then the guest prints a HBIOS/CBIOS version-mismatch warning.
Using another release's disks for data files only, without booting from them, is
fine.

### RomWBW Version

**There is no longer a single pinned release.** The version this emulator
reports to the guest is read out of the loaded ROM's HBIOS Configuration
Block at run time, so one binary boots any release listed in
`ROMWBW_SUPPORTED_RELEASES` ([`src/romwbw_pin.h`](src/romwbw_pin.h)) - today
v3.5.1 and v3.6.0. `romwbw_emu --version` prints the list:

```
RomWBW releases this build can run: 3.5.1, 3.6.0
  (the version a guest sees is read from the ROM it loads, not compiled in)
```

Five things report a version to the guest, and all five now derive it from
the ROM rather than from a constant: `HBF_SYSVER`, the NVRAM checksum seed,
the HBIOS ident block, the CBIOS page-zero stamp at `0x42`/`0x43`, and the
load-time check itself. Two of those exist only in emulated RAM, where no
verifier that reads bytes out of a built ROM can see them.

What is still refused is a release nobody has checked this core against.
Banks 1-15 of an `emu_*.rom` are upstream RomWBW, but bank 0 is ours, backed
by a C++ dispatcher implementing a specific set of HBIOS functions; a release
whose CBIOS calls something it does not implement would load and then hang.
`--allow-untested-romwbw` overrides that with a warning.

**The pairing rule did not go away.** A ROM and the boot slice of a disk
image still have to be the same release, or the guest prints

```
*** WARNING: HBIOS/CBIOS Version Mismatch ***
```

That warning is now the only thing enforcing it, since the emulator will
happily load either release. To check a tree before shipping a build, or
when a downloaded image misbehaves:

```bash
./roms/verify_romwbw_pin.sh
```

It reads every ROM and image it can find - in this tree, and in the `romwbw-get`
cache - checks the pairing between them and the built binary's `--version` list
against `src/romwbw_pin.h`, and exits non-zero listing anything that disagrees.
With nothing anywhere it still passes and exits 0 - the pin header and the
binary are the part it could check - but the PASS line claims only that part,
and a second line says `NO ARTIFACT WAS INSPECTED`. That is the normal state of a
fresh clone; fetch something and run it again to check what actually landed.
`make -C src test` runs it, and it takes a tree root, so
`./roms/verify_romwbw_pin.sh ../z80cpmw` checks what a downstream port is about
to ship.

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

### Reading an Image with `cpm_disk.py`

`cpm_disk.py` is the family's CP/M image tool, and the only one this repository
uses. It is a single stdlib-only Python file owned by
[cpmemu](https://github.com/avwohl/cpmemu) at `util/cpm_disk.py`; there is no
copy here, and there should not be one. Point `$CPM_DISK` at it, put a cpmemu
checkout beside this one, or run cpmemu's `make install`, which puts it on PATH
as `cpm_disk`.

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

The format is auto-detected from the file size - 8,388,608 bytes is a plain
hd1k slice, 51,380,224 is a combo - so there is **no diskdefs file to pick, no
`-T logical`, and no libdsk**. `--slice N` addresses any of a combo's six
slices; the tool reads the whole file and indexes into it, so slice 5 is no
harder than slice 0.

This used to be a section about cpmtools, and the hazards it described were
real. cpmtools 2.23 cannot be configured without libdsk; its default path
double-counts `boottrk` on a diskdef that carries no `offset`, and it fails
silently rather than loudly. Measured against the published `hd1k_infocom`,
whose directory sits at 16384: `cpmls -f wbw_hd1k` listed nothing at all, and
`cpmcp` exited 0 having written at 32768 - into the data area, over a file that
was already there. Picking the diskdef was left to the caller, and picking wrong
printed a plausible-looking garbage directory, which reads as "the file is not
on this image": that is how `hd1k_infocom.img` was once recorded as carrying no
`w8.com` when it carries both. And libdsk cannot address past 8 MB from the
start of a file, so combo slices 1-5 were out of reach on every packaged build,
homebrew's as much as Debian's.

None of that applies to `cpm_disk.py`, so `disks/diskdefs` went with the
cpmtools recipes that needed it. romwbw_disks still uses cpmtools to *build*
the published images, and keeps its own `tools/diskdefs` for that.

`ComboDisk` is a subclass of `Hd1kDisk` (cpmemu `566fd00`) — a slice is a plain
hd1k image at an offset, so there is one implementation of the format rather
than two. A file of any size that fits the slice round-trips; one that would run
past the end of the disk is refused rather than growing the image.

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

ROM boot, two disks - `hd1k_combo` in `--disk0`, `hd1k_infocom` in `--disk1`,
`--boot=C` - so four slices each:

```
A:=MD0:0	B:=MD1:0	C:=HDSK0:0	D:=HDSK0:1	E:=HDSK0:2
F:=HDSK0:3	G:=HDSK1:0	H:=HDSK1:1	I:=HDSK1:2	J:=HDSK1:3
```

Disk boot, one disk - `hd1k_combo` in `--disk0` with `--boot=2` - so eight
slices, and the slice that was booted is `A:`:

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
the probe bytes are present in the `w8.com` it assembles from `src/w8.asm`, and
in the `w8.com` on any image it is pointed at.

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

**Availability:** `r8.com` and `w8.com` are on slice 0 of the published `hd1k_combo` image, the only one in either catalog with `host_transfer` set - `tools/romwbw-get list` prints `R8/W8` beside it. Images from anywhere else - a downstream port's bundled copy, an older release asset - carry whatever `r8.com`/`w8.com` they were built with, and are not updated by a change here. `disks/verify_disk_utils.sh` is what checks them: it assembles both utilities from `src/r8.asm` and `src/w8.asm` and asserts the `HBF_HOST_CAPS` interlock is still in the `w8.com` that comes out, which needs no network and no image, and then inspects any `*.img` in a directory you name, defaulting to the `romwbw-get` cache - so it checks the bytes actually downloaded rather than what the catalog says about them. `make -C src test` runs it. Building the published images, and asserting the same interlock across all of them, is [romwbw_disks](https://github.com/avwohl/romwbw_disks)' job now.

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

`make serve` mirrors `emu_avw` and `hd1k_combo` into `web/` before it starts the
server, so a local serve comes up with a ROM and a bootable disk in its selects
rather than an empty list - which needs a network the first time and nothing
after that.

The page has no hardcoded ROM or disk names any more. It builds its RomWBW, ROM
and disk lists at load time from `catalog/manifest.json` beside it, written by
`romwbw-get mirror`, and checks each download against the size and SHA-256 the
manifest carries. A page with no mirror says so and still loads your own ROM and
disk images through its file pickers. Why a mirror and not a fetch straight from
GitHub - a browser physically cannot do the latter - is in
[docs/CATALOG.md](docs/CATALOG.md).

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

Other targets: `make test` runs the four C++ suites, the three node JS suites
and the Python catalog-client test when node and `python3` are on `PATH`
(skipped, not failed, when they are not), and both shell verifiers,
`disks/verify_disk_utils.sh` and `roms/verify_romwbw_pin.sh`. None of it needs a
network: `tests/catalog_client_test.py` serves a fixture catalog to
`tools/romwbw-get` on 127.0.0.1, and the two verifiers check whatever artifacts
happen to be on the machine and say so when there are none. `make install` and
`make uninstall` honour `PREFIX` (default `/usr/local`) and `DESTDIR`;
`make STATIC=1` links statically, which is what the release workflow builds.

For WebAssembly see [WebAssembly Version](#webassembly-version) above; that
build needs emscripten and the same sibling cpmemu checkout.

## Features

- **Memory:** 512KB ROM + 512KB RAM with 32KB bank switching
- **HBIOS:** Hardware abstraction layer implemented in C++
- **Disks:** ROM disk, RAM disk, and file-backed hard disk images
- **Disk Formats:** Auto-detects hd1k and hd512 RomWBW formats
- **ROM and disk images:** Not shipped with the program - `tools/romwbw-get` fetches them from the [romwbw_disks](https://github.com/avwohl/romwbw_disks) catalog and checks every byte against the published SHA-256 ([docs/CATALOG.md](docs/CATALOG.md))
- **Console:** Raw mode, so every control character reaches the guest; the VT emulation is the host terminal's job (xterm.js in the browser)
- **File Transfer:** R8/W8 utilities copy files between the host and CP/M (CLI paths or browser picker/download)
- **Settings file:** A JSON machine description in place of a long command line
- **Debugger:** A `sim>` prompt with breakpoints, single-step, register and memory dumps and a symbol table (there is no disassembler - `dm` prints bytes)
- **RomWBW releases:** Boots v3.5.1 and v3.6.0 from one binary - the version is read from the loaded ROM, not compiled in (`roms/verify_romwbw_pin.sh`)
- **WebAssembly:** Run RomWBW in any modern browser

## Boot Configuration

The emulator supports automatic boot configuration via NVRAM. Settings persist across sessions in `$XDG_CONFIG_HOME/romwbw_emu/nvram` (default `~/.config/romwbw_emu/nvram`), a plain text file containing a single line such as `C` or `2.3`.

### Quick Boot Examples

```bash
# Auto-boot to CP/M (ROM app 'C')
tools/romwbw-get run -- --boot=C

# Auto-boot from the first hard disk (unit 2), slice 0
tools/romwbw-get run -- --boot=2

# Auto-boot unit 2 (the first hard disk), slice 3
tools/romwbw-get run -- --boot=2.3

# Show boot menu (default)
tools/romwbw-get run -- --boot=H
```

`romwbw-get run` attaches the catalog's slot-0 disk as `--disk0` unless you pass
`--no-disks`, so `--boot=2` has something to boot from. Spelled out, without the
fetcher in the way:

```bash
rom="$(tools/romwbw-get path @rom)"
disk="$(tools/romwbw-get path --work @disk0)"
src/romwbw_emu --romwbw="$rom" --disk0="$disk" --boot=2.3
```

### Using SYSCONF (Interactive Configuration)

Type `W` and Enter at the boot menu to reach the RomWBW SYSCONF utility. This is
RomWBW 3.6.0, the release `romwbw-get` defaults to, started with `--boot=H` -
which is why the boot option it shows is `H`:

```
Boot [H=Help]: W

RomWBW System Config Utility, Version 1.1 June-2025

Current Configuration: 
  [BO] / Boot Options: ROM (App = "H")
  [AB] / Auto Boot: Enabled (Timeout = 0)

Commands:
  (P)rint - Display Current settings
  (S)et {SW} {val}[,{val}[,{val}]]- Set a switch value(s)
  (R)eset - Init NVRAM to Defaults
  (H)elp [{SW}] - This help menu, or help on a switch
  e(X)it - Exit Configuration

$
```

3.5.1 prints `Version 1.0 Nov-2024` and offers `(Q)uit - Quit` where 3.6.0
offers `e(X)it - Exit Configuration`; both releases take either letter. 3.5.1
also announces `Loading RomWBW Configure...` first and comes back to a fresh
boot loader banner on the way out, where 3.6.0 returns straight to the
`Boot [H=Help]:` prompt.

Neither release's help spells out the switch syntax - `H BO` and `H AB` print
that, with examples. At the `$` prompt:

| Command | What it does |
|---------|--------------|
| `P` | Print the current configuration |
| `S BO D,u,s` | Boot from disk unit `u`, slice `s` |
| `S BO R,app` | Boot ROM application `app` (`C` = CP/M 2.2, `Z` = Z-System) |
| `S AB E,t` | Enable autoboot with a `t` second countdown |
| `S AB E,0` | Enable autoboot with no countdown |
| `S AB D` | Disable autoboot |
| `R` | Init NVRAM to defaults |
| `X` or `Q` | Exit configuration |

Every `S` prints the whole configuration back, so it doubles as a `P`.

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

`--boot` takes what you would have typed at the menu, and there are ten ROM
applications to pick from, not two: `M` Monitor, `C` CP/M 2.2, `Z` Z-System,
`B` BASIC, `T` Tasty BASIC, `F` Forth, `P` Play a Game, `N` Network Boot,
`X` XModem Flash Updater, `U` User App. 3.5.1 lists them under `L`; 3.6.0
answers `L` with `*** Invalid command` and folds them into the `H` menu, which
also carries `O` (hardware monitor) and `S` (slice inventory). The menu itself
also takes `D` (device inventory), `R` (reboot), `W` (SYSCONF), `I <u> [<c>]`
(console interface and baud rate) and `V [<n>]` (HBIOS diagnostic verbosity) -
each of them a line, terminated by Enter.

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
src/romwbw_emu --romwbw="$(tools/romwbw-get path @rom)" \
               --disk0="$(tools/romwbw-get path --work @disk0)" \
               --boot=2 --save-config
src/romwbw_emu   # boots the saved machine
```

`--save-config` writes the paths as they were resolved, so the saved file names
the cached ROM and the working copy of the disk directly and never consults the
catalog again. `romwbw-get` keeps its own release and artifact choice in a
**separate** file, `$XDG_CONFIG_HOME/romwbw_emu/catalog.json`, rather than in
this one: `--save-config` rebuilds this document out of the C++ struct, so a key
the loader does not know would be dropped the first time anybody saved.

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
`[CONFIG]` banner, and then the run fails with `Error: no ROM given, and no
"rom" in a settings file.` - which names the key that is missing but not the one
that is there instead. `--save-config` is the way to see the schema the loader
actually reads.

Two smaller details. `XDG_CONFIG_HOME` is honoured only when it is absolute; a
relative value falls back to `~/.config` silently. And `--config=FILE` beats
`--no-config` no matter what order they appear in - `--no-config` suppresses
discovery, not an explicitly named file.

## Examples

```bash
# Fetch the defaults and boot - the short way
tools/romwbw-get run

# Boot from ROM disk (default), driving the emulator directly
src/romwbw_emu --romwbw="$(tools/romwbw-get path @rom)"

# Boot with a hard disk attached
src/romwbw_emu --romwbw="$(tools/romwbw-get path @rom)" \
               --disk0="$(tools/romwbw-get path --work @disk0)"

# Two disks attached (this is the ROM-boot map under Drive Letters)
src/romwbw_emu --romwbw="$(tools/romwbw-get path @rom)" \
               --disk0="$(tools/romwbw-get path --work hd1k_combo)" \
               --disk1="$(tools/romwbw-get path --work hd1k_infocom)"
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
    romwbw_pin.h		The RomWBW releases this core can run
    version.cc			Version and build stamp
    r8.asm w8.asm		The CP/M file-transfer utilities
    emu_hbios.asm		The Z80 HBIOS stub inside the emulator ROM
  tests/			4 C++, 3 node JS and 1 Python suite (make -C src test)
  web/
    romwbw.html-template	The page; romwbw.html is rendered from it
    romwbw_web.cc		WebAssembly front end
    vendor/			Vendored xterm, so the page needs no CDN
  tools/
    romwbw-get			Fetches and verifies the ROM and the disk images
  roms/				build_emu_rom.sh and verify_romwbw_pin.sh - no ROM is tracked
  disks/			verify_disk_utils.sh - no image, no diskdefs
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
- [docs/CATALOG.md](docs/CATALOG.md) - Where the ROM and the disk images come from, how `romwbw-get` verifies them, and why they are not in this repository
- `docs/BOOT_CONFIGURATION.md` - Boot options, SYSCONF utility, NVRAM persistence
- [docs/CONFIGURATION.md](docs/CONFIGURATION.md) - The JSON settings file and its schema
- `docs/DISK_FORMATS.md` - Disk formats, SIMH compatibility, and cpmtools usage
- `docs/disk-images.md` - Working with the disk images by hand
- `docs/drive_assignment.md` - How CBIOS builds the drive map, and what the emulator has to report for it to work
- `docs/ARCHITECTURE.md` - Emulator architecture and the shared C++ HBIOS implementation
- `docs/HBIOS_Implementation_Guide.md` - How HBIOS is implemented
- `docs/HBIOS_DATA_EXPORTS.md` - HBIOS data structures
- `docs/ROM_ATTESTATION.md` - The rights the `emu_avw` ROM is distributed under - bank 0 this project's, banks 1-15 RomWBW's - written for App Store review, with a v1.40 note on where the ROM lives now that none is tracked here
- [docs/RELEASE_ORDER_2026-08-25.md](docs/RELEASE_ORDER_2026-08-25.md) - The order the ports were asked to release in for the `W8` host-path work. Historical: its commands name scripts and images v1.40 removed

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

