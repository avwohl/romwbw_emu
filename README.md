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

At the RomWBW boot menu, press `2` to boot from disk, or `C` for CP/M from ROM.

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

Download disk images from [RomWBW releases](https://github.com/wwarthen/RomWBW/releases) (in the Package.zip).

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

## WebAssembly Version

Try RomWBW in your browser - no installation required:

```bash
cd web && make
# Open romwbw.html in a browser, or:
make serve   # Start local server at http://localhost:8000
```

Load your own ROM and disk images through the web interface.

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
- **WebAssembly:** Run RomWBW in any modern browser

## Command Line Options

```
./romwbw_emu --romwbw=<rom.rom> [options]

Options:
  --romwbw=FILE     Enable RomWBW mode with ROM file
  --debug           Enable debug output
  --strict-io       Halt on unexpected I/O ports

Disk options:
  --disk0=FILE      Attach disk image to slot 0 (drives C:-F:)
  --disk1=FILE      Attach disk image to slot 1 (drives G:-J:)

Other options:
  --escape=CHAR     Console escape char (default ^E)
  --trace=FILE      Write execution trace
  --symbols=FILE    Load symbol table (.sym)
```

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

- `docs/DISK_FORMATS.md` - Disk formats, SIMH compatibility, and cpmtools usage
- `docs/ROMWBW_INTEGRATION.md` - RomWBW architecture and HBIOS details
- `docs/HBIOS_Implementation_Guide.md` - How HBIOS is implemented
- `docs/HBIOS_DATA_EXPORTS.md` - HBIOS data structures
- `docs/IOS_README.md` - iOS/WebAssembly disk support notes

## Related Projects

- [RomWBW](https://github.com/wwarthen/RomWBW) - Z80/Z180 ROM-based system software
- [cpmemu-bdos](https://github.com/avwohl/cpmemu-bdos) - CP/M BDOS translator for Linux
- [ioscpm](https://github.com/avwohl/ioscpm) - iOS/macOS version
- [z80cpmw](https://github.com/avwohl/z80cpmw) - windows version

## License

GNU General Public License v3.0 - see [LICENSE](LICENSE).
