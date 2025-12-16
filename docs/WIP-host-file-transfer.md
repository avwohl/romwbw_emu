# WIP: Host File Transfer (R8/W8 Utilities)

## Status: Debugging disk loading issue

### What's Done
1. **emu_io abstraction for host files** - Added to emu_io.h:
   - `emu_host_file_open_read/write()`
   - `emu_host_file_read_byte/write_byte()`
   - `emu_host_file_close_read/write()`

2. **WASM implementation** (emu_io_wasm.cc):
   - `js_host_file_request_read()` - triggers file picker
   - `js_host_file_download()` - triggers browser download
   - Exported functions: `emu_host_file_load()`, `emu_host_file_cancel()`

3. **JavaScript handlers** (romwbw.html):
   - `Module.onHostFileRequestRead` - opens file picker, calls `emu_host_file_load()`
   - `Module.onHostFileDownload` - triggers download

4. **hbios_dispatch.cc** - Updated HOST_CLOSE to use emu_io abstraction

5. **R8.COM and W8.COM** - Built and added to hd1k_games.img and hd1k_cpm22.img

### Current Issue
Disk loaded via file picker shows as HDSK1 instead of HDSK0, even when loaded into Disk 0 slot. This causes no drive letter to be assigned (C: expects HDSK0).

Debug logging added to:
- `romwbw_load_disk()` in romwbw_web.cc
- `populateDiskUnitTable()` in hbios_dispatch.cc

### Next Steps
1. Check browser console for debug output to see which slot disk is actually loaded into
2. Fix the disk slot assignment if needed
3. Test R8/W8 once disks mount correctly

### Files Modified
- src/emu_io.h - host file API
- src/emu_io_wasm.cc - WASM implementation
- src/emu_io_cli.cc - CLI implementation
- src/hbios_dispatch.cc - HOST_CLOSE fix + debug logging
- src/hbios_dispatch.h - removed old file handle members
- web/romwbw.html - JS handlers for file picker/download
- web/romwbw-debug.html - same JS handlers
- web/romwbw_web.cc - debug logging
- web/makefile - added exported functions
