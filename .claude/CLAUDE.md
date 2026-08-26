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
  - **Never write `org 0100h` in a CP/M `.COM` source.** L80 bases a relocatable
    code segment at 0100h by itself, so an ORG is applied *on top of* that base
    and puts the code at 0200h behind 256 zero bytes. The result runs - CP/M
    loads the whole file at 0100h and the Z80 slides through 256 NOPs into the
    code - which is why `src/w8.asm` carried one unnoticed for a long time,
    wasting 256 bytes of every copy and making the binary uncomparable with
    `src/r8.asm`, which had none. Build a `.COM` with no ORG at all:
    ```
    um80 -o w8.rel ../src/w8.asm && ul80 -o w8.com w8.rel
    ```
    (The `-p` origin flag does not help: it names where the image is *loaded*,
    not where the segment starts. The `-p 0000` above is for a ROM image, which
    genuinely does start at address zero.)

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

**cpmtools: pick the right diskdef, and do not set DISKDEFS**

`disks/diskdefs` in this repository is the definitive one, and both disk
scripts `cd` into `disks/` so that cpmtools picks it up. Do not rely on the
system file: cpmtools reads `./diskdefs` OR the system copy, never both, and
what the system copy holds depends on the distribution. This file used to say
the stock file "already defines the whole `wbw_hd1k` family including the
per-slice `wbw_hd1k_0..3`. There is nothing to export." That is true of
homebrew and false of Debian and Ubuntu, whose cpmtools 2.23 has `wbw_hd1k`
and no per-slice definition at all - and it is what once sent a reading of
this astray and turned CI red for a file that was on the image. Nobody's
copy is upstream's: the 2.23 tarball from moria.de ships 139 diskdefs and
not one of them mentions RomWBW, so every `wbw_*` definition anywhere is a
packager's addition and has to be checked rather than assumed. Setting
`DISKDEFS` to a path that does not exist is *silently ignored*, so the wrong
instruction looks like it works.

`disks/diskdefs` now carries `wbw_hd1k` and `wbw_hd1k_0..5`, all six slice
definitions checked against hd1k_combo.img. Its comment header records the
one thing worth knowing before using the higher slices: cpmtools 2.23 cannot
be built without libdsk, and its libdsk backend cannot address anything past
8 MB from the start of the image, so on a Debian or Ubuntu cpmtools
`wbw_hd1k_1` and up answer "cannot read superblock (Bad parameter)" and even
`wbw_hd1k_0` cannot reach the last 1 MB of slice 0. That is a limit of the
build, not of the definitions; a device_posix build of the same sources reads
all six.

```bash
cpmls -f wbw_hd1k   disks/hd1k_infocom.img   # plain 8 MB image
cpmls -f wbw_hd1k_0 disks/hd1k_combo.img     # combo: slice 0, past the 1 MB MBR
cpmcp -f wbw_hd1k_0 disks/hd1k_combo.img 0:w8.com ./w8.com   # extract
cpmrm -f wbw_hd1k_0 disks/hd1k_combo.img 0:w8.com            # cpmcp will NOT
cpmcp -f wbw_hd1k_0 disks/hd1k_combo.img ./w8.com 0:w8.com   # overwrite
```

The `wbw_hd1k_0` definition is what handles the combo image's 1 MB prefix, so
the dd slice-extract-and-write-back dance this file used to prescribe is not
needed. **The real hazard is the one worth keeping:** the wrong diskdef does not
fail, it prints a garbage directory, which reads as "no such file". That is
exactly how `hd1k_infocom.img` was once recorded as carrying no `w8.com` when it
carries both.

For `r8.com` / `w8.com` specifically, do not do any of this by hand:
`disks/rebuild_disk_utils.sh` assembles both and installs them into every
tracked image, and `disks/verify_disk_utils.sh` (run by `make -C src test`)
checks that the images match the sources.
