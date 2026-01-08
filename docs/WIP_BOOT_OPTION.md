# WIP: Boot Option Implementation

**Date:** 2026-01-07
**Status:** Partially working - mechanism complete, ROM content issue discovered

## Summary

The `--boot=CMD` option is implemented and working. Input is correctly queued and consumed by the romldr. However, after processing the boot command, the romldr enters a silent loop because **the SBC_simh_std.rom doesn't have standard OS loader images in banks 2-4** (it has Forth code instead).

## What Was Implemented

### 1. `--boot=CMD` Command-Line Option (COMPLETE)

**Files changed:**
- `src/romwbw_emu.cc` - Added parsing and queueing logic
- `src/emu_init.cc` - Added ROM app bank initialization
- `src/emu_init.h` - Added function declaration
- `src/hbios_dispatch.cc` - Added CIOOUT debug logging
- `src/hbios_dispatch.h` - Fixed duplicate function declaration

**Usage:**
```bash
./src/romwbw_emu --romwbw=roms/emu_avw.rom --boot=C    # Boot CP/M from ROM
./src/romwbw_emu --romwbw=roms/emu_avw.rom --boot=2    # Boot from disk
./src/romwbw_emu --romwbw=roms/emu_avw.rom --boot=H    # Show help
```

**How it works:**
1. Boot string is parsed from `--boot=` argument
2. Characters are queued to console input via `emu_console_queue_char()`
3. A CR is appended to submit the command
4. The romldr reads these characters via CIOIN and processes them

### 2. ROM App Bank Initialization (COMPLETE)

Added `emu_copy_rom_apps_to_ram()` in `emu_init.cc`:
- Reads CB_BIDAPP0 (0x1E0) and CB_APP_BNKS (0x1E1) from HCB
- Copies 32KB ROM banks 2, 3, 4 to RAM banks 0x89, 0x8A, 0x8B
- This is required because romldr expects OS images in RAM banks, not ROM

## Current Status

### What Works
- `--boot` option correctly parses and queues input
- Input IS consumed by romldr (verified via debug):
  ```
  [CIOIN] read char: 67 (0x43) 'C'
  [CIOIN] read char: 13 (0x0D) '?'
  ```
- ROM app banks ARE copied to RAM banks:
  ```
  [EMU_INIT] Copying 3 ROM apps: ROM banks 0x02+ -> RAM banks 0x89+
  [EMU_INIT]   App 0: ROM bank 0x02 -> RAM bank 0x89 (first byte: 0x21)
  ```

### What Doesn't Work
After the romldr reads the boot command (H, C, 2, etc.), **no output appears** and the system enters a loop. The romldr silently fails to execute the command.

## Root Cause Analysis

### Finding: SBC_simh_std ROM Doesn't Have Standard OS Loader Images

Examined ROM bank 2 (where first OS app should be):
```
00010000: 2180 fd2e 0025 f924 e5dd e125 25e5 fde1  !....%.$...%%...
00010010: 1101 00c3 6d17 0000 0004 4558 4954 dd5e  ....m.....EXIT.^
```

This is **Forth code**, not a CP/M loader. The strings "EXIT", "EXECUTE", "VARIABLE", "CONSTANT", "USER", "EMIT" visible in the hex dump confirm this.

### What Should Be There

Standard RomWBW ROMs have OS loader images in banks 2-4:
- Bank 2: CP/M 2.2 loader (CPM.SYS format)
- Bank 3: ZSDOS loader (ZSYS.SYS format)
- Bank 4: Optional third OS

Each OS image has a header with:
- Signature/name identifying the OS
- Load address, end address, entry point at offset 0x7Fxx

### Why the romldr Fails

1. romldr scans app banks looking for valid OS image signatures
2. Finds Forth code instead of OS loaders
3. No valid apps to display in help menu
4. No valid apps to boot when 'C' or 'Z' is typed
5. Boot from disk ('2') may also fail if no valid boot sector found

## Next Steps

### The Real Issue: SBC_simh_std.rom Content

The `roms/build_from_source.sh` script builds emu_avw.rom (the only ROM this emulator uses):
- Bank 0: Our emu_hbios.asm
- Banks 1-15: From RomWBW `SBC_simh_std.rom`

**Problem:** The SBC_simh_std.rom from RomWBW has Forth code in bank 2, not CP/M loader. This is likely because the SIMH variant is configured to boot from disk rather than embedded ROM apps.

**To investigate:**
1. Check RomWBW build config for SBC_simh - does it include ROM apps?
2. Try a different RomWBW ROM variant (e.g., `SBC_std.rom`) that includes embedded OS loaders
3. Or modify the RomWBW build to include CPM.SYS/ZSYS.SYS in banks 2-3

### Tested: Boot from Disk Also Fails

Tested with disk attached:
```bash
./src/romwbw_emu --romwbw=roms/emu_avw.rom --disk0=web/hd1k_cpm22.img --boot=2
```

Same behavior - input consumed, no output after. The romldr silently loops after processing any boot command (H, C, 2, etc.).

**Conclusion:** The romldr from SBC_simh_std.rom doesn't work correctly under our emulator. Either:
1. The romldr expects hardware/HBIOS behavior we don't emulate correctly
2. The SBC_simh variant has a special romldr that works differently
3. There's a bug in our HBIOS dispatch that breaks romldr after input

The `--boot` mechanism itself is working correctly - the issue is romldr compatibility.

## Files Modified Summary

| File | Changes |
|------|---------|
| `src/romwbw_emu.cc` | Added `--boot=` parsing, queueing, help text |
| `src/emu_init.cc` | Added `emu_copy_rom_apps_to_ram()` |
| `src/emu_init.h` | Added function declaration |
| `src/hbios_dispatch.cc` | Added CIOOUT debug logging |
| `src/hbios_dispatch.h` | Removed duplicate `getInitializedBanksBitmap()` |

## Debug Commands

To trace boot behavior:
```bash
# With debug output
timeout 3s ./src/romwbw_emu --romwbw=roms/emu_avw.rom --boot=C --debug 2>&1 | grep "CIOIN\|CIOOUT"

# Check what's in ROM bank 2
xxd -s 0x10000 -l 64 roms/SBC_simh_std.rom
```

## Build

```bash
cd src && make
```
