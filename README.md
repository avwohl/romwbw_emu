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

## Features

- **Memory:** 512 KB ROM + 512 KB RAM with 32 KB bank switching
- **HBIOS:** hardware abstraction layer implemented in C++
- **Disks:** ROM disk, RAM disk, and up to 16 file-backed hard disk images
- **Disk formats:** auto-detects hd1k and hd512, including 49 MB combo images
- **RomWBW releases:** boots any release the catalog publishes, from one
  binary, reading the version out of the ROM it loads
- **Operating systems:** CP/M 2.2, CP/M 3, ZSDOS, ZPM3, NZCOM and QPM, from the
  published disk images
- **Console:** raw mode, so every control character reaches the guest; the VT
  emulation is the host terminal's job (xterm.js in the browser)
- **File transfer:** `R8`/`W8` copy files between CP/M and the host
- **Settings file:** a JSON machine description in place of a long command line
- **Debugger:** a `sim>` prompt with breakpoints, single-step, register and
  memory dumps, and a symbol table - CLI only, and with no disassembler; `dm`
  prints bytes
- **Unattended runs:** `tools/romwbw-batch` boots CP/M 2.2, runs a list of
  commands from a submit file and extracts the results; `tools/romwbw-plm80`
  uses it to compile PL/M-80 with Intel's own compiler under DRI's ISX - see
  [docs/BATCH.md](docs/BATCH.md) and [docs/ISX.md](docs/ISX.md)
- **WebAssembly:** runs in any modern browser

## Quick Start

```bash
git clone https://github.com/avwohl/cpmemu ../cpmemu   # the Z80 core
make -C ../cpmemu/src libqkz80.a
make -C src                                            # build the emulator
tools/romwbw-get run                                   # fetch a ROM and a disk, verify them, and boot
```

The first two lines build the Z80 core, the one dependency a fresh clone is
missing. Anything after a bare `--` goes to the emulator:
`tools/romwbw-get run -- --boot=2.1`. At the boot menu, `2` boots the first
hard disk, `C` boots CP/M 2.2 from ROM, and `H` lists the commands.

## Installation

```bash
# Debian/Ubuntu (romwbw-emu_arm64.deb on ARM64)
curl -LO https://github.com/avwohl/romwbw_emu/releases/latest/download/romwbw-emu_amd64.deb
sudo dpkg -i romwbw-emu_amd64.deb
# Fedora/RHEL (romwbw-emu.aarch64.rpm on ARM64)
curl -LO https://github.com/avwohl/romwbw_emu/releases/latest/download/romwbw-emu.x86_64.rpm
sudo rpm -i romwbw-emu.x86_64.rpm

romwbw-get run
sudo romwbw-get mirror /usr/share/romwbw_emu/web   # ROMs and disks for the installed browser page
```

macOS has no package; build from source, which is what CI tests with Apple
clang. [docs/BUILDING.md](docs/BUILDING.md) covers building, the other ways to
supply the Z80 core, and everything the packages install.

## Documentation

Process files: [CHANGELOG.md](CHANGELOG.md) (what changed in each release, with
the commit behind every entry), [DOWNSTREAM.md](DOWNSTREAM.md) (what a port must
do to take a sync of this core; `docs/DOWNSTREAM_*.md` are the dated notices),
[MANUAL_CHECKS.md](MANUAL_CHECKS.md), [DECISIONS.md](DECISIONS.md) and
`todo.txt`.

- [docs/USAGE.md](docs/USAGE.md) - running the emulator: boot menu, `--boot`, command line options, keyboard, settings file, R8/W8
- [docs/ROMS_AND_DISKS.md](docs/ROMS_AND_DISKS.md) - ROMs and disk images, RomWBW releases, reading an image, drive letters
- [docs/BUILDING.md](docs/BUILDING.md) - building, requirements, what the packages install, the WebAssembly build, project structure
- [docs/CATALOG.md](docs/CATALOG.md) - where the ROM and the disk images come from, and how `romwbw-get` verifies them
- [docs/BOOT_CONFIGURATION.md](docs/BOOT_CONFIGURATION.md) - boot options, the boot menu, SYSCONF, NVRAM
- [docs/CONFIGURATION.md](docs/CONFIGURATION.md) - the JSON settings file and its schema
- [docs/FILE_TRANSFER.md](docs/FILE_TRANSFER.md) - R8/W8 in full
- [docs/BATCH.md](docs/BATCH.md) - `romwbw-batch`: CP/M commands with nobody at the keyboard
- [docs/ISX.md](docs/ISX.md) - Intel's ISIS-II tools under DRI's ISX, `romwbw-plm80`, and MP/M II rebuilt byte for byte
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
