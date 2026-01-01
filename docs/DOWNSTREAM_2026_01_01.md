# Downstream Emulator Updates - 2026-01-01

Changes made today that may require updates in downstream emulators (Windows, iOS, Mac).

## Shared Code Changes (should auto-propagate)

These changes are in shared files (`emu_init.cc`, `hbios_dispatch.cc`) and should work automatically if you pull the latest code:

### 1. Drive Letter Ordering (hbios_dispatch.cc)

Hard disks are now assigned drive letters BEFORE memory disks. This matches RomWBW behavior where boot disk slices are A:, B:, C:, D:, with RAM/ROM disks after.

**Before:** A:=MD0, B:=MD1, C:=HDSK0:0, D:=HDSK0:1...
**After:** A:=HDSK0:0, B:=HDSK0:1, C:=HDSK0:2, D:=HDSK0:3, E:=MD0, F:=MD1...

### 2. CBIOS Page Zero Stamp (emu_init.cc)

When initializing RAM banks, we now set up the CBIOS signature at 0x40-0x55. This is required for ASSIGN, MODE, and other RomWBW utilities to work in CP/M 3's banked environment.

```
0x40-0x43: Stamp ('W', ~'W' (0xA8), version 0x35, update 0x10)
0x44-0x45: CBX pointer (points to 0x0050)
0x50-0x51: DEVMAP pointer (0x0000 - unused)
0x52-0x53: DRVMAP pointer (0x0120 - HCB drive map)
0x54-0x55: DPBMAP pointer (0x0000 - unused)
```

This is handled in `emu_init_ram_bank()`.

## Platform-Specific Changes

### 3. Auto Slice Count Calculation

The CLI now auto-calculates slice count based on number of attached disks, matching CBIOS behavior:
- 1 hard disk: 8 slices
- 2 hard disks: 4 slices each
- 3+ hard disks: 2 slices each

**Action required:** If your UI allows attaching multiple disks, implement similar logic. The calculation is:

```cpp
int auto_slices = (disk_count <= 1) ? 8 : (disk_count == 2) ? 4 : 2;
```

Call `hbios->setDiskSliceCount(unit, auto_slices)` after loading disks.

### 4. Console I/O (emu_io_cli.cc - CLI only)

Fixed EOF handling and slow I/O. These changes are in `emu_io_cli.cc` which is Unix-specific. The WASM version (`emu_io_wasm.cc`) and other platform I/O implementations may need similar fixes if they have issues:

- **EOF handling:** Don't treat "no data available" as EOF on TTY. Use select() with no timeout to wait for actual input.
- **Input status check:** `emu_console_has_input()` should use 0 timeout (instant check), not 10ms delay which causes 300-baud-like slowdowns.

## Default Slice Count Change

The default `max_slices` in `HBDisk` struct (hbios_dispatch.h) is now 8 instead of 4. This only matters if you don't implement auto-calculation.

## Testing

After pulling changes, test:
1. Boot CP/M 3 (option 2.3) - should work
2. Run `ASSIGN` command - should show drives without "Unexpected CBIOS" error
3. Boot CP/M 2.2 - console I/O should be fast, no ^Z flood
4. Drive letters should put boot disk at A:, memory disks after HD slices
