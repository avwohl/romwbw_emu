# iOS/macOS/Windows Emulator Notes

This document tracks changes made to the romwbw_emu project that need to be ported to the iOS, macOS, and Windows emulator builds.

## Disk Format Changes

### Auto-Detection Logic

The emulator now auto-detects disk format. Implementation in `hbios_dispatch.cc`:

1. **Check for MBR signature** (0x55AA at offset 510)
   - Scan partition table for type 0x2E (CP/M) at offsets 0x1C2, 0x1D2, 0x1E2, 0x1F2
   - If found: combo disk with 1MB prefix

2. **Check file size**
   - Exactly 8,388,608 bytes (8MB): hd1k single-slice
   - Exactly 8,519,680 bytes (8.32MB): hd512 format
   - Other sizes with MBR: combo disk

3. **Fallback**: hd512 format

### Key Constants

```cpp
static constexpr size_t HD1K_SINGLE_SIZE = 8388608;      // 8 MB exactly
static constexpr size_t HD1K_PREFIX_SIZE = 1048576;      // 1 MB prefix for combo
static constexpr size_t HD1K_SLICE_SIZE = 8388608;       // 8 MB per slice
static constexpr size_t HD512_SINGLE_SIZE = 8519680;     // 8.32 MB
```

### Disk Geometry

**hd1k format:**
- 512 bytes/sector
- 64 sectors/track
- 16 tracks/cylinder (heads)
- 256 cylinders (for 8MB slice)
- 16,384 sectors per slice

**hd512 format:**
- 512 bytes/sector
- 32 sectors/track
- 16 tracks/cylinder
- 520 tracks total
- 16,640 sectors total

## Command Line Changes

### New Options
- `--disk0=<path>` - Primary disk (boots as C:)
- `--disk1=<path>` - Secondary disk (boots as D:)

### Validation
- Files must exist (no auto-creation on typos)
- File size must match known format (8MB, 8.32MB, or combo with 1MB prefix)

## Host File Transfer (R8/W8)

### Overview
R8.COM and W8.COM are CP/M utilities for transferring files between the emulated CP/M system and the host filesystem.

- **R8.COM** - Read file from host into CP/M
- **W8.COM** - Write file from CP/M to host

### HBIOS Functions Used

These utilities use HBIOS function 0xF8 (BF_SYSINT) with subfunctions:

| Subfunction | Name | Description |
|-------------|------|-------------|
| 0x00 | INTINF | Get emulator info |
| 0x01 | INTGET | Read file from host |
| 0x02 | INTPUT | Write file to host |
| 0x03 | INTGETB | Read binary from host |
| 0x04 | INTPUTB | Write binary to host |

### Implementation Required

The emulator needs to handle BF_SYSINT (0xF8) calls:

```cpp
case 0xF8: {  // BF_SYSINT - System integration
    uint8_t subfunc = cpu.c();
    switch (subfunc) {
        case 0x00:  // INTINF - Return emulator info
            // HL = version, DE = capabilities
            break;
        case 0x01:  // INTGET - Read text file from host
            // DE = filename buffer (null-terminated)
            // HL = destination buffer
            // BC = max length
            // Returns: A=0 success, HL=bytes read
            break;
        case 0x02:  // INTPUT - Write text file to host
            // DE = filename buffer
            // HL = source buffer
            // BC = length
            // Returns: A=0 success
            break;
        // ... similar for binary variants
    }
    break;
}
```

## Disk Images

### Recommended Disk
**hd1k_combo.img** (51,380,224 bytes) - Contains multiple OS slices:
- Slice 0: CP/M 2.2 with R8.COM and W8.COM
- Slice 1: ZSDOS
- Slice 2: CP/M 3 (ZPM3)
- Slice 3-5: Additional systems

### File Locations
- Source: `disks/hd1k_combo.img`
- Web deploy: `~/www/romwbw1/hd1k_combo.img`

### Adding Files to Combo Disk
Use `src/add_to_combo.py` (cpmtools doesn't support combo disk offset):

```bash
python3 src/add_to_combo.py disks/hd1k_combo.img file1.com file2.com
```

## GitHub Release (avwohl/ioscpm)

The iOS/macOS app fetches disk images from GitHub releases:
- **Catalog URL**: `https://github.com/avwohl/ioscpm/releases/latest/download/disks.xml`
- **Disk base URL**: `https://github.com/avwohl/ioscpm/releases/latest/download/`

### Current Release Assets (v1.1)

| File | Size | Contains R8/W8 |
|------|------|----------------|
| disks.xml | 1,431 bytes | N/A (catalog) |
| hd1k_combo.img | 51,380,224 bytes | Yes |
| hd1k_utils.img | 8,388,608 bytes | Yes |
| hd1k_cpm22.img | 8,388,608 bytes | No |
| hd1k_zpm3.img | 8,388,608 bytes | No |
| hd1k_zsdos.img | 8,388,608 bytes | No |

### Updating Release Assets

To update disk images in the release:

```bash
# Upload/replace assets in existing release
gh release upload v1.1 --repo avwohl/ioscpm --clobber hd1k_combo.img disks.xml

# Or create a new release
gh release create v1.2 --repo avwohl/ioscpm --title "v1.2" hd1k_combo.img disks.xml
```

### Adding Files to Combo Disk

Use the Python script (cpmtools doesn't support combo disk offset):

```bash
python3 src/add_to_combo.py disks/hd1k_combo.img r8.com w8.com
```

## Reference Files

- `src/romwbw_emu.cc` - Main emulator with HBIOS handlers
- `src/hbios_dispatch.cc` - HBIOS function dispatch
- `src/hbios_cpu.cc` - I/O port emulation
- `src/emu_io.h` - Platform I/O interface
- `src/emu_io_common.cc` - Shared portable I/O implementation
- `src/emu_init.cc` - Shared initialization
- `docs/DISK_FORMATS.md` - Detailed disk format documentation
- `src/r8.asm`, `src/w8.asm` - Z80 source for file transfer utilities
- `src/add_to_combo.py` - Python script to add files to combo disks
