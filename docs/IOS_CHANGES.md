# RomWBW Emulator Changes for Downstream Clients

**Date:** December 2024
**Affects:** iOS, WebAssembly, Mac builds

## Summary

Major improvements to RomWBW integration:
- ROM is now built from source instead of binary patching
- Disk slices now work correctly (8 slices per hard disk)
- Boot loader 'd' command shows device list
- Console output CR/LF handling fixed for Unix terminals

---

## ROM Changes

### Old Approach (Deprecated)
Previously we patched pre-built RomWBW ROMs by replacing bytes at specific offsets. This was fragile and broke frequently.

### New Approach
We now build a clean ROM by combining:
1. **Bank 0:** Our `emu_hbios.asm` (minimal HBIOS proxy with OUT 0xEF dispatch)
2. **Banks 1-15:** RomWBW's romldr, OS images, and ROM disk (from `SBC_simh_std.rom`)

See `roms/build_from_source.sh` for the build process.

### Action Required
- Use `roms/emu_avw.rom` instead of patched ROMs
- The ROM name convention is now `emu_avw.rom` (avw = author initials)

---

## Disk Slice Support

### What Changed
Hard disks now correctly report the "high capacity" attribute (bit 5 in device attributes), which tells CBIOS to assign multiple drive letters per disk.

### Result
A single hard disk image now gets 8 drive letters:
```
C:=HDSK0:0  (slice 0)
D:=HDSK0:1  (slice 1)
E:=HDSK0:2  (slice 2)
...
J:=HDSK0:7  (slice 7)
```

### Action Required
None - this is automatic. Disk images with multiple slices (like `hd1k_combo.img`) will now work correctly.

---

## Device List ('d' Command)

### What Changed
The boot loader's 'd' command now works. We trap `HB_BNKCALL` at address `0xFFF9` when `IX=0x0406` (PRTSUM vector) and display the device summary from C++.

### Output Format
```
Disk Device Summary

 Unit Dev       Type    Capacity
 ---- --------- ------- --------
    0 MD0       RAM      256KB
    1 MD1       ROM      384KB
    2 HDSK0     Hard      49MB
```

### Action Required
None - handled automatically by the emulator core.

---

## Console Output Changes

### What Changed
- CR characters (`\r`) are now filtered out in `emu_console_write_char()`
- CP/M sends `\r\n` but Unix terminals only need `\n`
- This prevents double-spaced output

### Action Required for Downstream Clients

If your platform already handles CR/LF conversion, you may want to adjust. The CLI implementation now does:

```cpp
void emu_console_write_char(uint8_t ch) {
  ch &= 0x7F;  // Strip high bit
  // CP/M sends \r\n, but Unix terminals only need \n
  if (ch != '\r') {
    putchar(ch);
    fflush(stdout);
  }
}
```

**iOS/Mac:** If your terminal view expects `\r\n`, you may need to restore CR or handle line endings differently.

**WebAssembly:** Browser consoles typically handle `\n` fine, so filtering CR should work.

---

## API Changes

### hbios_dispatch.cc

Fatal errors converted to graceful returns:
- `DIOSTATUS`, `DIOSEEK`, `DIOREAD`, `DIOWRITE`, `DIODEVICE`, `DIOMEDIA`, `DIOCAP` now return `HBR_NOUNIT` instead of calling `emu_fatal()` for unknown units
- This prevents crashes when probing non-existent disk units

### DIODEVICE Return Values

Now returns device attributes in register C:
```cpp
cpu->regs.BC.set_low(dev_attr);  // C = device attributes
// Bit 5 (0x20) = high capacity (multiple slices)
// Bit 6 (0x40) = removable media
```

### EXTSLICE File-Backed Disk Support

`EXTSLICE` now properly reads MBR from file-backed disks using the portable I/O layer:
```cpp
if (disk.file_backed && disk.handle) {
  size_t read = emu_disk_read((emu_disk_handle)disk.handle, 0, mbr, 512);
  // ... parse MBR
}
```

---

## Required emu_io.h Functions

Ensure your platform implements these (already in emu_io.h):

```cpp
// Disk I/O - used for MBR reading
emu_disk_handle emu_disk_open(const std::string& path, const char* mode);
size_t emu_disk_read(emu_disk_handle disk, size_t offset, uint8_t* buffer, size_t count);
size_t emu_disk_size(emu_disk_handle disk);
void emu_disk_close(emu_disk_handle disk);
```

---

## Testing Checklist

After updating, verify:

1. **Boot:** System boots to CP/M prompt
2. **Device list:** Press 'd' at boot menu, see MD0, MD1, HDSK0
3. **Slices:** `Configuring Drives...` shows C:-J: for hard disk
4. **No double-spacing:** Output is single-spaced (not blank lines between every line)
5. **Directory:** `dir` command works on each drive

---

## Files Modified

| File | Changes |
|------|---------|
| `src/romwbw_emu.cc` | HB_BNKCALL trap, PRTSUM handler, high capacity attribute |
| `src/emu_io_cli.cc` | CR filtering, OPOST disable |
| `src/hbios_dispatch.cc` | Graceful error handling, file-backed MBR reading |
| `roms/build_from_source.sh` | New build script |

---

## Questions?

Contact the maintainers or check the git history for detailed changes.
