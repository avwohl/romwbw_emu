# CP/M 2.2 Archive

This directory contains the old CP/M 2.2 emulation code that was replaced by the RomWBW HBIOS-based emulator.

## Contents

- `cpm22asm/` - Original CP/M 2.2 BIOS/BDOS assembly source, and `build_cpm.sh`
  which assembles it
- `cpm_bios.cc` - Old BIOS-level CP/M emulator (booted from disk images)
- `console_io.*` - Console I/O for old emulator
- `disk_image.*` - Disk image handling (.IMD, .dsk files)
- `diskdefs.*` - Disk format definitions
- `bios.asm` - WebAssembly BIOS for old web version
- `cpm_cli.cc`, `cpm_web.cc` - Old CLI and web versions
- `index.html` - Old web UI for CP/M 2.2
- `com/` - CP/M programs (mbasic.com)

## What was removed, and why it is still recoverable

This directory used to be the one place in the repository that still tracked
binaries after the v1.40 artifact migration. Six of the seven went on
2026-09-07, because each was either rebuildable from a source beside it or a
copy of something published elsewhere. All six are in git history if anything
here ever needs one back.

| Removed | Why |
|---|---|
| `Binary/SBC_std.rom` | byte-identical to the stock `Binary/SBC_std.rom` in the upstream RomWBW v3.5.1 `Package.zip` - verified with `cmp`. `tools/romwbw-get path @upstream` fetches that package, sha256-checked. |
| `Binary/HD0.img` | zero bytes. It was a placeholder for a disk image that was never committed. |
| `cpm_bios` | an x86-64 ELF built from `cpm_bios.cc` beside it - a host build artifact that should never have been committed at all. |
| `cpm22.sys`, `cpm22asm/cpm22.sys`, `cpm22asm/cpm22.bin` | built from `cpm22asm/cpm22.asm` by `cpm22asm/build_cpm.sh`, which is two lines of `um80`/`ul80` and is still here. |

`com/mbasic.com` stays, and is now the only tracked binary in the repository.
It is third-party CP/M software with no source in this tree and no upstream
this project controls, so deleting it would lose it outright.
`.github/workflows/test.yml`'s tracked-artifact guard names `archive/` as its
one exception for that file's sake.

## Current Emulator

The project now uses RomWBW with HBIOS implemented in C++ (`src/romwbw_emu.cc`, with the HBIOS implementation in `src/hbios_dispatch.cc` and the port interception in `src/hbios_cpu.cc`).
See the main README.md for current usage.
