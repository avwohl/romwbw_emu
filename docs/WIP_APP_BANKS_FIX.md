# WIP: App Banks Investigation - RESOLVED

## Original Problem
Boot menu only shows "Monitor" and "Z-System" instead of full OS list (expected CP/M 2.2, etc.)

## Investigation Results

### Finding 1: HCB Copy is Correct
- ROM[0x1E0-0x1E1] = 0x89 0x03 (CB_BIDAPP0=0x89, CB_APP_BNKS=3)
- RAM[0x1E0-0x1E1] = 0x89 0x03 (correctly copied)
- The HCB copy from ROM bank 0 to RAM bank 0x80 works correctly

### Finding 2: App Directory Analysis
The app directory is stored in ROM bank 1 at offset 0x0D50. Examining the structure:

```
0x8D50: Entry 1 - Key='M' (0x4D), Name pointer=0x8D77 -> "Monitor"
0x8D60: Entry 2 - Key='Z' (0x5A), Name pointer=0x8D88 -> "Z-System"
0x8D70: End of entries, start of name strings
```

The directory only contains **2 app entries**, not 3.

### Finding 3: CB_APP_BNKS vs App Count
- CB_APP_BNKS=3 means "3 ROM banks allocated for apps" (banks 0x09-0x0B)
- This does NOT mean "3 apps" - apps can span multiple banks
- The actual number of apps is determined by the app directory entries

### Finding 4: CP/M 2.2 String Location
The string "CP/M 2.2" exists at 0x8D7F in the ROM, but:
- It is NOT referenced by any app entry
- It sits between "Monitor" and "Z-System" strings
- It appears to be a leftover/unused string

### Conclusion: NOT A BUG

The SBC_simh_std configuration was intentionally built with only 2 ROM apps:
1. **M**: Monitor - Debug/diagnostic tool
2. **Z**: Z-System - ZCPR3/ZSDOS environment

CP/M 2.2 is available as a **disk boot**, not as a ROM app in this configuration.
To boot CP/M, users should:
1. Attach a disk image with CP/M: `--hbdisk0=disk.img`
2. At boot prompt, type the disk unit: `0` or `HD0`

### Verification
Both `SBC_simh_std.rom` and `emu_romwbw.rom` have identical app directories with only 2 entries. The emulator behavior is correct.

## Files Investigated
- `src/altair_emu.cc`: SYSSETBNK handler, HCB copy logic
- `src/romwbw_mem.h`: Bank memory layout (BANK_SIZE=32KB)
- `roms/SBC_simh_std.rom`: Original ROM
- `roms/emu_romwbw.rom`: Our ROM (identical app directory)

## Debug Output Added
Added SYSPEEK verbose logging to track bank reads:
```cpp
if (debug) {
  fprintf(stderr, "[SYSPEEK] bank=0x%02X addr=0x%04X -> 0x%02X\n", bank, addr, byte);
}
```

Also added HCB verification output during startup:
```cpp
fprintf(stderr, "  ROM[0x1E0-0x1E1] = 0x%02X 0x%02X (CB_BIDAPP0, CB_APP_BNKS)\n", rom[0x1E0], rom[0x1E1]);
fprintf(stderr, "  RAM[0x1E0-0x1E1] = 0x%02X 0x%02X (should match ROM)\n", ram[0x1E0], ram[0x1E1]);
```

## Status: CLOSED
The original assumption about missing apps was incorrect. The emulator correctly displays all 2 ROM apps defined in the ROM image.
