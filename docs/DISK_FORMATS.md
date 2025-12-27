# RomWBW Disk Formats

This document describes the disk formats supported by the romwbw_emu emulator and how to work with disk images using cpmtools.

## Quick Reference

| Format | Size | Sectors/Slice | Dir Entries | SIMH Equivalent |
|--------|------|---------------|-------------|-----------------|
| **hd1k single** | 8 MB (8,388,608 bytes) | 16,384 (0x4000) | 1024 | HDSK (compatible) |
| **hd1k combo** | 1MB + N×8MB | 16,384 per slice | 1024 | - |
| **hd512** | 8.32 MB (8,519,680 bytes) | 16,640 (0x4100) | 512 | HDCPM (compatible) |

## SIMH Compatibility

This emulator is compatible with disk images created by the SIMH AltairZ80 simulator. The table below shows which SIMH formats work:

### Supported SIMH Formats

| SIMH Format | File Size | Extension | Our Format | Status |
|-------------|-----------|-----------|------------|--------|
| **HDSK** (default hard disk) | 8,388,608 bytes (8 MB) | `.dsk`, `.img` | hd1k | **Works** |
| **HDCPM** (Amstrad hard disk) | 8,519,680 bytes (8.32 MB) | `.dsk`, `.img` | hd512 | **Works** |

### Not Supported

| SIMH Format | File Size | Reason |
|-------------|-----------|--------|
| **88-DISK** (8" floppy) | 337,568 bytes (~330 KB) | Uses 137-byte hard-sectored format |
| **Mini-disk** (5.25" floppy) | 76,720 bytes (~75 KB) | Uses 137-byte hard-sectored format |

### Why It Works

The SIMH HDSK format and our hd1k format are **binary compatible**. Both store data as sequential sectors from byte 0:

- SIMH HDSK: 2048 tracks × 32 sectors × 128 bytes = 8,388,608 bytes
- RomWBW hd1k: 1024 tracks × 16 sectors × 512 bytes = 8,388,608 bytes

The geometry differs but the byte layout is identical. CP/M doesn't care about physical geometry - it uses logical block addressing at the BIOS level.

### Using SIMH Disk Images

```bash
# 8 MB SIMH hard disk image - works directly
./romwbw_emu --romwbw roms/emu_romwbw.rom --disk0=cpm3.dsk

# Check file size first
ls -l myimage.dsk
# If exactly 8,388,608 bytes → treated as hd1k
# If exactly 8,519,680 bytes → treated as hd512
```

### Quick Size Reference

To determine if a `.dsk` or `.img` file will work:

```bash
ls -l *.dsk *.img
```

| Size (bytes) | Size (readable) | Format | Compatible? |
|--------------|-----------------|--------|-------------|
| 8,388,608 | 8.0 MB exactly | HDSK/hd1k | Yes |
| 8,519,680 | 8.1 MB (~8.32 MB) | HDCPM/hd512 | Yes |
| 51,380,224 | 49 MB (1MB + 6×8MB) | RomWBW combo | Yes (native) |
| 337,568 | 330 KB | 88-DISK floppy | No |
| 76,720 | 75 KB | Mini-disk floppy | No |

**Note:** The 51MB combo disk format is RomWBW-native (not a SIMH format). It contains a 1MB MBR prefix followed by six 8MB slices, each of which uses the same sector layout as SIMH HDSK.

### Obtaining SIMH Disk Images

SIMH CP/M disk images can be found at:
- Peter Schorn's site: https://schorn.ch/altair.html
- The `cpmplus.zip` distribution includes bootable CP/M 3 images
- Various CP/M archives with `.dsk` files sized at 8 MB

## Setting Up cpmtools

cpmtools requires a `diskdefs` file with format definitions. RomWBW provides this file.

### Environment Setup

Add to your `~/.bashrc`:

```bash
# RomWBW diskdefs for cpmtools
export DISKDEFS="$HOME/esrc/RomWBW-v3.5.1/Tools/cpmtools/diskdefs"
```

Then reload: `source ~/.bashrc`

### Verify Setup

```bash
cpmls -f wbw_hd1k somefile.img
```

If you get "unknown format" errors, the DISKDEFS path is wrong.

## Format Details

### hd1k (Modern Format - Recommended)

The hd1k format is the modern RomWBW standard with 1024 directory entries.

**Diskdef: `wbw_hd1k`**
```
diskdef wbw_hd1k
  seclen 512
  tracks 1024
  sectrk 16
  blocksize 4096
  maxdir 1024
  skew 0
  boottrk 2
  os 2.2
end
```

**Geometry:**
- 512 bytes/sector
- 16 sectors/track
- 1024 tracks
- 4096 bytes/block (8 sectors)
- 1024 directory entries
- 2 boot tracks (reserved)
- Total: 1024 × 16 × 512 = 8,388,608 bytes (exactly 8 MB)

### hd1k Combo Disk (Multi-Slice)

Combo disks have a 1 MB MBR prefix followed by multiple 8 MB slices.

**Structure:**
```
Offset          Size    Content
0x00000000      1 MB    MBR prefix (partition type 0x2E at offset 0x1C2)
0x00100000      8 MB    Slice 0
0x00900000      8 MB    Slice 1
0x01100000      8 MB    Slice 2
...
```

**Diskdefs for combo slices:**
- `wbw_hd1k_0` - offset 128T (slice 0, after 1MB prefix)
- `wbw_hd1k_1` - offset 1152T (slice 1)
- `wbw_hd1k_2` - offset 2176T (slice 2)
- `wbw_hd1k_3` - offset 3200T (slice 3)

Formula: `offset (tracks) = 128 + (1024 × slice_num)`

### hd512 (Legacy Format)

The older RomWBW format with 512 directory entries.

**Diskdef: `wbw_hd512`**
- 1040 tracks × 16 sectors × 512 bytes = 8,519,680 bytes (8.32 MB)
- 512 directory entries
- 16 boot tracks

## Emulator Auto-Detection

The emulator automatically detects disk format:

1. **MBR Check**: Reads sector 0, looks for signature 0x55AA at offset 510-511
2. **Partition Scan**: If MBR valid, scans for partition type 0x2E (RomWBW hd1k)
3. **Size Check**: If no 0x2E partition but size = exactly 8 MB, assumes hd1k single-slice
4. **Fallback**: Otherwise assumes hd512 format

**For auto-detect to work:**
- Single-slice hd1k: must be exactly 8,388,608 bytes
- Multi-slice combo: must have valid MBR with partition type 0x2E

## Working with Disk Images

### Creating a New Disk

```bash
# Create blank 8MB image
dd if=/dev/zero bs=1024 count=8192 of=mydisk.img

# Format as hd1k
mkfs.cpm -f wbw_hd1k mydisk.img
```

### Copying Files TO a Disk

```bash
# Copy single file to user 0
cpmcp -f wbw_hd1k mydisk.img myfile.com 0:

# Copy multiple files
cpmcp -f wbw_hd1k mydisk.img *.com 0:

# Copy to user area 1
cpmcp -f wbw_hd1k mydisk.img myfile.com 1:
```

### Copying Files FROM a Disk

```bash
# Copy from user 0
cpmcp -f wbw_hd1k mydisk.img 0:myfile.com ./

# Copy all .com files from user 0
cpmcp -f wbw_hd1k mydisk.img "0:*.com" ./
```

### Listing Directory

```bash
# List all files
cpmls -f wbw_hd1k mydisk.img

# Long format with sizes
cpmls -l -f wbw_hd1k mydisk.img
```

### Working with Combo Disks

```bash
# List slice 0 of a combo disk
cpmls -f wbw_hd1k_0 hd1k_combo.img

# Copy file to slice 1
cpmcp -f wbw_hd1k_1 hd1k_combo.img myfile.com 0:
```

### Deleting Files

```bash
cpmrm -f wbw_hd1k mydisk.img 0:oldfile.com
```

## Disk Image Sources

### Original RomWBW Images

The original disk images from RomWBW v3.5.1:
```
~/esrc/RomWBW-v3.5.1/Binary/hd1k_*.img
```

These are unmodified reference images.

### Project Disk Locations

| Location | Purpose | Notes |
|----------|---------|-------|
| `disks/` | Development/testing | May have local modifications |
| `~/www/romwbw/` | Web deployment (original) | Copy of RomWBW originals |
| `~/www/romwbw1/` | Web deployment (modified) | With R8/W8 utilities added |

### Verifying Image Integrity

```bash
# Compare to RomWBW original
md5sum myimage.img ~/esrc/RomWBW-v3.5.1/Binary/hd1k_cpm22.img
```

## Command Line Usage

### Current Options (romwbw_emu)

```bash
# Attach disk to unit 0 (appears as C:)
./romwbw_emu --romwbw roms/emu_romwbw.rom --hbdisk0=disks/hd1k_combo.img

# Multiple disks
./romwbw_emu --romwbw roms/emu_romwbw.rom \
    --hbdisk0=disks/hd1k_combo.img \
    --hbdisk1=disks/hd1k_games.img
```

### Disk Unit Mapping

| Option | HBIOS Unit | Drive Letters |
|--------|------------|---------------|
| --hbdisk0 | HD0 (unit 2) | C: D: E: F: (4 slices) |
| --hbdisk1 | HD1 (unit 3) | G: H: I: J: (4 slices) |

Note: Units 0-1 are MD0/MD1 (RAM/ROM disks), so hard disks start at unit 2.

### Legacy SIMH Protocol (--hdsk)

The `--hdsk0`, `--hdsk1` options use the SIMH HDSK port 0xFD protocol. This is a separate disk system from HBIOS and is not recommended for normal use.

## Troubleshooting

### "Unknown format" from cpmtools

Set the DISKDEFS environment variable:
```bash
export DISKDEFS="$HOME/esrc/RomWBW-v3.5.1/Tools/cpmtools/diskdefs"
```

### Disk not detected / wrong format

- Verify file size matches expected format
- For combo disks, verify MBR has partition type 0x2E at offset 0x1C2
- Run emulator with `--debug` to see format detection messages

### Can't write to disk

- Check file permissions
- Verify disk image isn't mounted elsewhere
- For read-only source images, copy first: `cp original.img working.img`

### Files copied but not visible in CP/M

- Ensure you copied to user area 0: `cpmcp -f wbw_hd1k disk.img file.com 0:`
- Check you're using correct diskdef for the image type
- For combo disks, use slice-specific diskdef (e.g., `wbw_hd1k_0`)
