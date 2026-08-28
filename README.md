# RomWBW Emulator

[![GitHub Release](https://img.shields.io/github/v/release/avwohl/romwbw_emu)](https://github.com/avwohl/romwbw_emu/releases/latest)
[![Build Status](https://img.shields.io/github/actions/workflow/status/avwohl/romwbw_emu/release.yml)](https://github.com/avwohl/romwbw_emu/actions/workflows/release.yml)

A hardware-level Z80 emulator for running RomWBW and CP/M from ROM and disk images. Features full Z80 CPU emulation with 512KB ROM + 512KB RAM bank switching and HBIOS hardware abstraction.

## Quick Start

```bash
# Build the emulator
cd src && make

# Run RomWBW (boots to ROM disk)
./romwbw_emu --romwbw=../roms/emu_avw.rom

# Run with a hard disk image
./romwbw_emu --romwbw=../roms/emu_avw.rom --disk0=../disks/hd1k_combo.img
```

At the RomWBW boot menu, press `2` to boot from the first hard disk, or `C` for CP/M from ROM. Boot units 0 and 1 are the on-board RAM and ROM memory disks and carry no operating system, so typing `0` reports `No system image on disk`; the first `--disk0` image is unit 2 (additional disks are units 3, 4, ...). Press `D` at the boot menu to list disk units, `L` to list ROM applications, and `W` to save your choice as the autoboot default.

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

### From Source

See [Building](#building) below.

## Disk Images

The emulator supports RomWBW hard disk images in both **hd1k** (modern) and **hd512** (classic) formats. Format is auto-detected from the MBR partition table.

### Recommended Disk Images

| Image | Size | Description |
|-------|------|-------------|
| `hd1k_combo.img` | 49MB | Multi-slice combo disk with CP/M 2.2 and utilities |
| `hd1k_games.img` | 8MB | Classic games: Colossal Cave, Castle, Dungeon |
| `hd1k_infocom.img` | 8MB | Infocom text adventures: Zork 1-3, Hitchhiker's Guide |
| `hd1k_cpm22.img` | 8MB | CP/M 2.2 system disk |
| `hd1k_zsdos.img` | 8MB | ZSDOS system disk |

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

| File Size | Format | Works? |
|-----------|--------|--------|
| 8,388,608 bytes (8 MB) | SIMH HDSK / hd1k | Yes |
| 8,519,680 bytes (8.32 MB) | SIMH HDCPM / hd512 | Yes |
| 51,380,224 bytes (49 MB) | RomWBW combo (native) | Yes |

See `docs/DISK_FORMATS.md` for details.

### Drive Letters

- `A:` - RAM disk (MD0)
- `B:` - ROM disk (MD1)
- `C:` - First hard disk (--disk0)
- `D:` - Second hard disk (--disk1)

## File Transfer (R8/W8)

The `R8` and `W8` CP/M utilities (sources: `src/r8.asm`, `src/w8.asm`) copy files between the host and CP/M. They talk to the emulator through HBIOS extension functions 0xE1-0xEA.

**CLI:** `R8 <hostpath>` imports a host file into CP/M. The path is used as typed - relative paths resolve against the directory `romwbw_emu` was started from, and absolute paths work. CP/M's CCP uppercases the command line, so the emulator retries host paths case-insensitively (typing `R8 /home/me/file.txt` works even though CP/M delivers `/HOME/ME/FILE.TXT`). The CP/M-side name is the uppercased 8.3 basename.

`W8 <cpmfile> [hostpath]` exports a CP/M file to the host. With no `hostpath` it lands in the emulator's working directory under the CP/M name, lowercased - what it always did. With one, it goes there instead. Both utilities take **the whole rest of the command line** as the path, so a directory whose name contains a space works: `W8 OUT.TXT /Users/me/My Documents/out.txt`. Trailing spaces are trimmed.

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

`W8` copies the file whole. It drops only the run of `^Z` characters at the very end, which is the padding CP/M writes into the last record of a file - so a text file imported with `R8` comes back byte for byte, and a `.COM` containing `1Ah` bytes (`LD A,(DE)`, which is common) is no longer truncated at the first one. The one thing that cannot survive is a file whose real content *ends* in `1Ah`: CP/M stores no length, only whole 128-byte records, so nothing can tell those bytes from the padding and they are dropped with it. Write errors at close (e.g. host disk full) are reported: `Host file close failed - file may be truncated`.

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
cd web && make
# Open romwbw.html in a browser, or:
make serve   # Start local server at http://localhost:8080
```

Load your own ROM and disk images through the web interface.

In the browser, `R8` imports files via a file picker and `W8` exports them as downloads - see [File Transfer (R8/W8)](#file-transfer-r8w8).

The web UI remembers your control selections (ROM choice, disk selections, slice counts, boot string, and the "don't warn" checkboxes) in browser localStorage, so they survive page reloads; clearing the browser's site data resets them to defaults. The Debug checkbox is deliberately not persisted, and local file uploads cannot be restored by the browser, so those revert to defaults on reload.

## Building

```bash
cd src/
make           # Build romwbw_emu
```

**Requirements:** C++11 compiler (gcc/clang), POSIX system (Linux/macOS)

For WebAssembly:
```bash
cd web/
make           # Requires emscripten
```

## Features

- **Memory:** 512KB ROM + 512KB RAM with 32KB bank switching
- **HBIOS:** Hardware abstraction layer implemented in C++
- **Disks:** ROM disk, RAM disk, and file-backed hard disk images
- **Disk Formats:** Auto-detects hd1k and hd512 RomWBW formats
- **Console:** Full terminal emulation with escape sequences
- **File Transfer:** R8/W8 utilities copy files between the host and CP/M (CLI paths or browser picker/download)
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

Press `W` at the boot menu to access the RomWBW SYSCONF utility:

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

| Format | Description |
|--------|-------------|
| `--boot=C` | Boot ROM app C (CP/M 2.2) |
| `--boot=Z` | Boot ROM app Z (ZSDOS) |
| `--boot=2` | Boot first hard disk (unit 2), slice 0 |
| `--boot=2.3` | Boot unit 2 (first hard disk), slice 3 |
| `--boot=H` | Show boot menu |
| `--boot=none` | Forget the persisted boot target and show the menu (`off` is the same) |

A `--boot` given on the command line applies to **that run only** and is not
written back, so a script cannot change what you boot by default. A boot target
the guest sets with `SYSCONF` during the run still is. `--boot=none` is the way
to undo one.

Boot unit numbers: 0 = RAM disk, 1 = ROM disk, 2 and up = hard disks in `--disk0`, `--disk1`, ... order. Press `D` at the boot menu to list them.

## Command Line Options

```
./romwbw_emu --romwbw=<rom.rom> [options]

Options:
  --romwbw=FILE     Enable RomWBW mode with ROM file
  --boot=CMD        Auto-boot command (C, Z, 2, 2.3, H, etc.), this run only
  --boot=none       Forget the persisted boot target ('off' is the same)
  --debug           Enable debug output
  --strict-io       Halt on unexpected I/O ports

Disk options:
  --disk0=FILE      Attach disk image to slot 0 (drives C:-F:)
  --disk1=FILE      Attach disk image to slot 1 (drives G:-J:)

Other options:
  --escape=CHAR     Key reserved for console mode (default ^E)
  --escape=none     Reserve no key; every byte reaches CP/M
  --trace=FILE      Write execution trace
  --symbols=FILE    Load symbol table (.sym)

Settings file:
  --config=FILE     Load settings from a JSON file
  --no-config       Ignore auto-discovered settings files
  --save-config[=F] Write the effective settings as JSON and exit

NVRAM persistence:
  NVRAM is persisted to $XDG_CONFIG_HOME/romwbw_emu/nvram (default ~/.config/romwbw_emu/nvram)
  Use SYSCONF (W at boot menu) to configure interactively.
```

## Keyboard

Every control character goes to the guest, because CP/M software uses them:
`^R` retypes the current line at the CCP prompt, `^E`/`^S`/`^D`/`^X` are the
WordStar cursor diamond, `^Q` starts the WordStar `^Qx` commands, `^O` starts
the `^Ox` onscreen-format commands, and `^C` warm-boots. The emulator claims no
Ctrl-letter for itself, with one exception.

**The escape character is reserved by the emulator and never reaches CP/M.**
`--escape=CHAR` names the key that suspends the guest and drops you at the
`sim>` prompt, where `help` lists the debugger commands and `quit` exits. The
default is `^E`, which is WordStar cursor-up, so if you run WordStar, VDE or
anything else built on that layout, either move the key (`--escape=^]`) or turn
it off entirely with `--escape=none`. `CHAR` is `^A` through `^_`, a literal
character, or `none` (`off` and `^@` mean the same); with `none` there is no
way into `sim>` at all and every control character reaches the guest. The same
value can live in a settings file as `"escape": "none"` — see
[docs/CONFIGURATION.md](docs/CONFIGURATION.md).

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

## Examples

```bash
# Boot from ROM disk (default)
./romwbw_emu --romwbw=roms/emu_avw.rom

# Boot with hard disk attached
./romwbw_emu --romwbw=roms/emu_avw.rom --disk0=disks/hd1k_combo.img

# Boot with tools disk
./romwbw_emu --romwbw=roms/emu_avw.rom --disk0=disks/z80cpm_tools.img
```

## Project Structure

```
romwbw_emu/
├── src/
│   ├── romwbw_emu.cc   # Main emulator with HBIOS and disk support
│   ├── romwbw_mem.h    # Bank-switched memory (512KB ROM + 512KB RAM)
│   ├── hbios_dispatch.*# HBIOS service handlers
│   └── emu_io*         # I/O abstraction layer (CLI/WASM)
├── web/
│   ├── romwbw.html     # RomWBW web interface
│   └── romwbw_web.cc   # WebAssembly emulator
├── roms/               # ROM images and build scripts
├── disks/              # Disk images
└── docs/               # Technical documentation
```

## Documentation

- `docs/BOOT_CONFIGURATION.md` - Boot options, SYSCONF utility, NVRAM persistence
- `docs/DISK_FORMATS.md` - Disk formats, SIMH compatibility, and cpmtools usage
- `docs/ARCHITECTURE.md` - Emulator architecture and the shared C++ HBIOS implementation
- `docs/HBIOS_Implementation_Guide.md` - How HBIOS is implemented
- `docs/HBIOS_DATA_EXPORTS.md` - HBIOS data structures

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

