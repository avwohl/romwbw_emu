# CPMEMU Project Rules

## RomWBW Integration

When working on RomWBW integration:

1. **NO HARDWARE I/O PORTS** - The emulator does not emulate hardware I/O ports. Do not write code that uses IN/OUT instructions to communicate with the emulator.

2. **NO RST TRAPS** - Do not rely on RST 08 or any other RST instruction to trap into the emulator. The Z80 code runs natively.

3. **HBIOS IS IN C++** - The HBIOS implementation lives entirely in the C++ emulator code (altair_emu.cc). The emulator provides HBIOS services by intercepting calls, not by running Z80 HBIOS code.

4. **BUILD A ROMWBW ROM** - The goal is to build a RomWBW ROM image that:
   - Uses our C++ HBIOS implementation instead of Z80 driver code
   - Contains the RomWBW boot loader, OS images, and ROM disk
   - Does NOT contain hardware-probing driver code

5. **STUDY THE BUILD SYSTEM** - Understand how RomWBW builds ROMs and how to configure it to exclude hardware drivers while keeping the useful parts (boot loader, OS, ROM disk).

## Build Tools

- **Z80 Assembler**: Use `um80` for assembling Z80 code. Do NOT use pasmo, z80asm, or other assemblers.
  - Add `.z80` directive at start of file to enable Z80 instructions
  - Assemble: `um80 -g file.asm` (creates file.rel in same directory as source)
  - Link: `ul80 -o output.bin -p 0000 file.rel`
  - Example for emu_hbios:
    ```
    cd roms
    um80 -g ../src/emu_hbios.asm
    ul80 -o emu_hbios.bin -p 0000 ../src/emu_hbios.rel
    dd if=/dev/zero bs=32768 count=1 of=emu_hbios_32k.bin
    dd if=emu_hbios.bin of=emu_hbios_32k.bin conv=notrunc
    ./build_emu_rom.sh SBC_simh_std.rom
    ```

## Emulator Architecture

- `qkz80` - Z80/8080 CPU emulator
- `banked_mem` / `romwbw_mem.h` - Bank-switched memory (512KB ROM + 512KB RAM)
- `romwbw_emu.cc` - Main emulator with HBIOS service handlers
- HBIOS calls are handled by intercepting execution at specific addresses and reading/writing CPU registers directly from C++

## Disk Formats (IMPORTANT)

**Read `docs/DISK_FORMATS.md` before working on disk-related code.**

Key points:
- **hd1k format**: 8MB (8,388,608 bytes exactly) single-slice, or combo with 1MB MBR prefix
- **hd512 format**: 8.32MB (8,519,680 bytes) legacy format
- The emulator auto-detects format by:
  1. Checking for MBR signature (0x55AA) and partition type 0x2E
  2. If no MBR but size = exactly 8MB, assumes hd1k single-slice
  3. Otherwise falls back to hd512

**Common pitfalls:**
- Single-slice hd1k images MUST be exactly 8,388,608 bytes for auto-detect
- Combo disks need the 1MB MBR prefix with partition type 0x2E at offset 0x1C2
- Use `--disk0` for disk images (e.g., `--disk0=disks/hd1k_combo.img`)
- cpmtools needs DISKDEFS env var pointing to RomWBW diskdefs file

**cpmtools setup:**
```bash
export DISKDEFS=/etc/cpmtools/diskdefs   # has wbw_hd1k (no wbw_hd1k_0 def)
cpmls -f wbw_hd1k disk.img               # single slice
# combo disks: no wbw_hd1k_0 def in /etc diskdefs - extract slice 0 instead:
dd if=combo.img bs=1M skip=1 count=8 of=slice0.img
cpmls -f wbw_hd1k slice0.img
dd if=slice0.img of=combo.img bs=1M seek=1 conv=notrunc   # write back
```
