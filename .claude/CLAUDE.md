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
- `altair_emu.cc` - Main emulator with HBIOS service handlers
- HBIOS calls are handled by intercepting execution at specific addresses and reading/writing CPU registers directly from C++
