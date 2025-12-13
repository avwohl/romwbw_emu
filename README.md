# RomWBW Emulator

A hardware-level Z80 emulator for running RomWBW and CP/M from ROM and disk images. Features full Z80 CPU emulation with 512KB ROM + 512KB RAM bank switching and HBIOS hardware abstraction.

## Quick Start

```bash
# Build the emulator
cd src && make

# Run RomWBW with the included ROM
./romwbw_emu --romwbw ../roms/emu_romwbw.rom

# Or use a standard RomWBW ROM with disk image
./romwbw_emu --romwbw ../roms/SBC_simh_std.rom --hbdisk0=~/path/to/hd512.img
```

At the RomWBW boot menu, press a key to select an OS (C for CP/M, Z for ZSDOS, etc.) or wait for auto-boot.

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

**Requirements:** C++11 compiler (gcc/clang), POSIX system (Linux)

For WebAssembly:
```bash
cd web/
make           # Requires emscripten
```

## Features

- **Memory:** 512KB ROM + 512KB RAM with 32KB bank switching
- **HBIOS:** Hardware abstraction layer implemented in C++
- **Disks:** ROM disk, RAM disk, and up to 16 virtual hard disk images
- **Console:** Full terminal emulation with escape sequences
- **Interrupts:** Maskable (INT) and non-maskable (NMI) support
- **WebAssembly:** Run RomWBW in any modern browser

## Command Line Options

```bash
./romwbw_emu --romwbw <rom.rom> [options]

Options:
  --hbdiskN=FILE    Attach disk image N (0-15)
  --romldr=FILE     Use boot loader from ROM
  --romapp=K:FILE   Register ROM app with boot key K
  --strict-io       Halt on unexpected I/O ports
  --debug           Enable debug output
  --trace=FILE      Write execution trace
```

## Getting RomWBW Images

Download pre-built ROM and disk images from the [RomWBW project](https://github.com/wwarthen/RomWBW):

```bash
# Clone RomWBW for pre-built binaries
git clone https://github.com/wwarthen/RomWBW.git

# ROM images are in Binary/
ls RomWBW/Binary/*.rom

# Disk images are in Binary/
ls RomWBW/Binary/*.img
```

The `SBC_std.rom` and `hd512_combo.img` work well with this emulator.

## Project Structure

```
romwbw_emu/
├── src/
│   ├── romwbw_emu.cc   # Main emulator
│   ├── qkz80.*         # Z80/8080 CPU core
│   ├── romwbw_mem.h    # Bank-switched memory
│   ├── hbios_dispatch.*# HBIOS service handlers
│   ├── emu_io*         # I/O abstraction layer
│   └── emu_hbios.asm   # HBIOS entry points
├── web/
│   ├── romwbw.html     # RomWBW web interface
│   ├── romwbw_web.cc   # WebAssembly emulator
│   └── Makefile        # WebAssembly build
├── roms/               # ROM images and build scripts
├── disks/              # Disk images
├── archive/cpm22/      # Legacy CP/M 2.2 emulator code
└── docs/               # Technical documentation
```

## Documentation

- `docs/ROMWBW_INTEGRATION.md` - RomWBW architecture and HBIOS details
- `docs/HBIOS_Implementation_Guide.md` - How HBIOS is implemented
- `docs/HBIOS_DATA_EXPORTS.md` - HBIOS data structures
- `docs/IOS_PORTING.md` - iOS/WebAssembly porting notes

## Related Projects

- [cpmemu-bdos](https://github.com/avwohl/cpmemu-bdos) - CP/M BDOS translator for running .com files on Linux
- [RomWBW](https://github.com/wwarthen/RomWBW) - Z80/Z180 ROM-based system software

## License

GNU General Public License v3.0 - see [LICENSE](LICENSE).
