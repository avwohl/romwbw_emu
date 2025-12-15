# WIP: WebAssembly Boot Failure Debug

**Date:** 2024-12-14
**Status:** In Progress

## Problem

WebAssembly version fails to boot with "Disk I/O failure" after displaying volume info.

### Console Output
```
[HBIOS] Loaded disk 0: 8388608 bytes (in-memory)
[WASM] RomWBW Emulator built Dec 14 2025 20:14:13 starting
[HBIOS EXTSLICE] Detected hd1k format (8MB single slice)
[HBIOS EXTSLICE] unit=0x02 slice=0 -> media=0x0A LBA=0
[HBIOS DIO] func=0x12 unit=2
[HBIOS DIO SEEK] HD0 (raw=2) lba=2
[HBIOS DIO] func=0x13 unit=2
[HBIOS HD READ] hd_unit=0 lba=2 count=1 buf=0x9000 bank=0x8E data.size=8388608
[HBIOS HD READ] completed: blocks_read=1 new_lba=3
[HBIOS DIO] func=0x12 unit=2
[HBIOS DIO SEEK] HD0 (raw=2) lba=3
<-- STOPS HERE - no DIOREAD for system image -->
```

### Screen Output
```
Volume "Games" [0xD000-0xFE00, entry @ 0xE600]
Disk I/O failure
```

## What Works

1. Disk loading - 8MB hd1k_games.img loaded correctly
2. EXTSLICE - Detects hd1k format, returns correct LBA=0
3. First SEEK (LBA 2) - Works
4. First READ (volume header) - Works, romldr displays volume info
5. Second SEEK (LBA 3) - Works

## What Fails

After SEEK to LBA 3, romldr should issue DIOREAD to load the system image (~25 sectors starting at 0xD000). But NO READ call ever happens.

## Key Finding

The `[OUT 0xEF]` logging we added does NOT appear in output, but `[HBIOS DIO]` logs DO appear. This means:
- HBIOS dispatch is NOT going through OUT 0xEF port handler
- Instead, it's using PC trapping (checkTrap() at line 144 in romwbw_web.cc)

## Latest Change

Added `[TRAP] PC=0xXXXX B=0xXX` logging before each trap-based dispatch to see:
1. What PC address triggers traps
2. Whether trapping is used for DIO calls
3. What happens after SEEK to LBA 3 returns

## Files Modified (uncommitted)

- `web/romwbw_web.cc` - Added debug logging:
  - Build timestamp at startup
  - `[OUT 0xEF]` logging (not firing - dispatch uses trapping)
  - `[TRAP]` logging for PC trap dispatch

- `src/hbios_dispatch.cc` - Added `[HBIOS DIO] func=0xXX unit=X` logging at start of handleDIO()

## Next Steps

1. Test with new `[TRAP]` logging to confirm PC trapping is in use
2. If trapping, check why trapping_enabled is true (set via signal port 0xEE with value 0xFF)
3. Trace CPU execution after SEEK to LBA 3 returns - where does it go?
4. Compare with CLI version which works correctly

## Hypothesis

The CLI version has explicit handling for HB_BNKCALL at 0xFFF9 (used by romldr). The WASM version might be missing this trap, causing romldr to execute invalid code after the SEEK.

Check `romwbw_emu.cc` lines 1565-1577 for the HB_BNKCALL trap in CLI version.

## Test Environment

- ROM: emu_avw.rom (emulator HBIOS)
- Disk: hd1k_games.img (8MB, single slice)
- URL: ~/www/romwbw1/
