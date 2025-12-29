# FIXED: CP/M 3 Hang After "RAM Disk Initialized"

## Status: FIXED (2025-12-26)
The issue was caused by an outdated/broken ROM file in the web directory.

## Root Cause
The web directory (`web/emu_avw.rom`) had a different ROM than the working CLI version (`roms/emu_avw.rom`).

- **Broken ROM hash**: `1515cc7d2fbef7c01f781fc7813592e745dc429d25534352616534a138a3bbca`
- **Working ROM hash**: `75990ada9ff6e61547b6c5f7f20bfd28036f2e1ca6709d172bad7fa261cc9682`

The broken ROM caused CP/M 3's BIOS (code at PC=0xFBE5 in BNKBIOS3) to write FCB data ("PROFILE S  ") to address 0x0000-0x000B, which corrupted the RST 08 vector. This led to a HLT at 0x0070/0x0071 when the next HBIOS call was attempted.

## Fix Applied
Copied the correct ROM from `roms/emu_avw.rom` to `web/emu_avw.rom`.

## Verification
The CLI was tested with both ROMs:
- **Broken ROM**: HLT at 0x0070 after page zero corruption
- **Working ROM**: CP/M 3 boots successfully to A> prompt

## Action Required
The deployed version at `https://www.awohl.com/romwbw1/` needs to be updated with the correct ROM file.

## Previous Investigation Notes
The page zero corruption trace showed:
```
[STORE_BANK] 0x8D:0x0000 = 0x00 (was 0xC3) PC=0xFBE5  <- Corruption starts!
[STORE_BANK] 0x8D:0x0001 = 0x50 (was 0x03) PC=0xFBE5  <- 'P' in PROFILE
[STORE_BANK] 0x8D:0x0002 = 0x52 (was 0xF6) PC=0xFBE5  <- 'R'
... (FCB data for "PROFILE S  ")
[STORE_BANK] 0x8D:0x0008 = 0x20 (was 0xC3) PC=0xFBE5  <- ' ' (corrupts RST 08!)
```

This was NOT an emulator bug - it was the broken ROM's BIOS code incorrectly writing to page zero.
