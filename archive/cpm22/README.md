# CP/M 2.2 Archive

This directory contains the old CP/M 2.2 emulation code that was replaced by the RomWBW HBIOS-based emulator.

## Contents

- `cpm22asm/` - Original CP/M 2.2 BIOS/BDOS assembly source
- `cpm22.sys` - CP/M 2.2 system file
- `cpm_bios.cc` - Old BIOS-level CP/M emulator (booted from disk images)
- `console_io.*` - Console I/O for old emulator
- `disk_image.*` - Disk image handling (.IMD, .dsk files)
- `diskdefs.*` - Disk format definitions
- `bios.asm` - WebAssembly BIOS for old web version
- `cpm_cli.cc`, `cpm_web.cc` - Old CLI and web versions
- `index.html` - Old web UI for CP/M 2.2
- `com/` - CP/M programs (mbasic.com)
- `examples/` - Configuration files for old emulator
- `Binary/` - Old SIMH-related ROMs and images
- `mydisk.img`, `drivec.img` - Old disk images

## Current Emulator

The project now uses RomWBW with HBIOS implemented in C++ (`src/cpmemu.cc`).
See the main README.md for current usage.
