# iOS/WebAssembly Disk Support Notes

This document describes the RomWBW disk format support added for iOS and WebAssembly builds.

## Overview

The emulator now supports RomWBW hard disk images, enabling CP/M software to be loaded from disk files rather than just the ROM disk. This is essential for running games and applications that don't fit in the ROM.

## Disk Format Support

### hd1k Format (Modern)

The modern RomWBW disk format introduced in v3.x:

- **Prefix**: 1MB (2048 sectors) before the RomWBW partition
- **Partition Type**: 0x2E in MBR partition table
- **Slice Size**: 8MB (16,384 sectors)
- **System Area**: 16KB (32 sectors) per slice
- **Directory Entries**: 1024 per slice
- **Media ID**: MID_HDNEW (0x0A)

### hd512 Format (Classic)

The original RomWBW disk format:

- **Prefix**: None (MBR at sector 0)
- **Slice Size**: 8.3MB (16,640 sectors)
- **System Area**: 128KB (256 sectors) per slice
- **Directory Entries**: 512 per slice
- **Media ID**: MID_HD (0x04)

## Auto-Detection

The emulator automatically detects disk format:

1. **MBR Check**: Reads sector 0 and looks for partition type 0x2E
2. **If 0x2E found**: Uses hd1k format with partition start LBA
3. **If file is exactly 8MB**: Assumes single-slice hd1k format
4. **Otherwise**: Falls back to hd512 format

## Implementation Details

### EXTSLICE Function (0xE0)

The key HBIOS function for disk support. Returns:
- **B**: Device attributes (0x00 = LBA mode)
- **C**: Media ID (0x04=hd512, 0x0A=hd1k)
- **DE:HL**: 32-bit LBA offset for the requested slice

Slice LBA calculation:
```
LBA = partition_base_lba + (slice_number * slice_size)
```

### HBDiskState Structure

Added fields for partition caching:
```cpp
struct HBDiskState {
  // ... existing fields ...
  bool partition_probed;      // True if MBR has been parsed
  uint32_t partition_base_lba; // Start of RomWBW partition
  uint32_t slice_size;        // Sectors per slice
  bool is_hd1k;               // True for hd1k format
};
```

## Recommended Disk Images for iOS

For bundling with the iOS app, these 8MB single-slice images are recommended:

| Image | Contents |
|-------|----------|
| `hd1k_games.img` | Classic games: Colossal Cave, Castle, Dungeon, Hitchhiker's |
| `hd1k_infocom.img` | Infocom text adventures: Zork 1-3, H2G2, Enchanter trilogy |

These are self-contained bootable images that work out of the box.

## Usage

### Command Line

```bash
# Boot from games disk
./romwbw_emu --romwbw emu_romwbw.rom --hbdisk0=hd1k_games.img --boot=2

# Boot from Infocom disk
./romwbw_emu --romwbw emu_romwbw.rom --hbdisk0=hd1k_infocom.img --boot=2
```

### WebAssembly/iOS

Pass disk image data to the emulator initialization:
```cpp
emulator->attach_hbios_disk(0, disk_image_path);
```

Then auto-boot with "2" to boot from the disk.

## Drive Mapping

After boot:
- **A:** RAM disk (MD0)
- **B:** ROM disk (MD1)
- **C:** First hard disk (hbdisk0)
- **D:** Second hard disk (hbdisk1)

## Testing

Verified working:
- `hd1k_combo.img` (49MB) - Detects 0x2E partition at LBA 2048
- `hd1k_infocom.img` (8MB) - Detects as hd1k by file size
- `hd1k_games.img` (8MB) - Detects as hd1k by file size
- Zork 1, 2, 3 - All playable from Infocom disk
- Colossal Cave Adventure - Playable from games disk

## Source Files Modified

- `src/romwbw_emu.cc`:
  - `HBDiskState` struct: Added partition/slice tracking fields
  - `EXTSLICE` case (0xE0): MBR parsing and format detection
  - `attach_hdsk_disk()`: SIMH HDSK port 0xFD support
  - `hdsk_write()`, `hdsk_execute_command()`: HDSK protocol

## References

- [RomWBW Hard Disk Anatomy](https://www.awohl.com/romdoc/Hard%20Disk%20Anatomy.pdf)
- [RomWBW System Guide](https://www.awohl.com/romdoc/RomWBW%20System%20Guide.pdf)
- [RomWBW GitHub](https://github.com/wwarthen/RomWBW)
