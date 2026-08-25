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

- **R8.COM** - Read file from host into CP/M (`R8 <hostpath>`)
- **W8.COM** - Write file from CP/M to host (`W8 <cpmfile> [hostpath]`)

Both take the whole rest of the command line as the path, so a directory name may contain spaces; trailing spaces are trimmed. W8 does not echo the path it was given - it asks the emulator where the file actually landed (HBF_HOST_GETNAME, below) and prints that, because on every front end except the CLI-with-an-exact-case-path the two are different strings.

W8 copies the file whole, dropping only the trailing run of `^Z` - the padding CP/M writes into a file's last record. It used to stop at the first `^Z`, which silently truncated every binary export (`W8.COM` itself came out 368 bytes of 1408).

Z80 sources: `src/r8.asm`, `src/w8.asm`. Neither has an `ORG`: M80 assembles each as one relocatable code segment and L80 bases a `.COM` at 0100h already, so an `org 0100h` in the source is applied on top of that and puts the code at 0200h behind 256 NOPs. `disks/verify_disk_utils.sh` checks the images against the sources, `disks/rebuild_disk_utils.sh` refreshes them.

### HBIOS Functions Used

The utilities call custom HBIOS extension functions 0xE1-0xE8 using the standard RomWBW calling convention (function code in B, invoked via `rst 8` in the guest). Source of truth: the enum in `src/hbios_dispatch.h` and the dispatch cases in `src/hbios_dispatch.cc`. The dispatcher does the actual host I/O through the `emu_host_file_*` platform API declared in `src/emu_io.h` (implemented per platform in `emu_io_cli.cc`, `emu_io_wasm.cc`, and downstream `emu_io_windows.cpp`).

- **HBF_HOST_OPEN_R (0xE1)** - Open host file for reading. DE = address of a null-terminated path string in guest memory. Returns A = 0 on success, 0xFF on failure.
- **HBF_HOST_OPEN_W (0xE2)** - Open host file for writing. DE = address of a null-terminated path string. Returns A = 0 on success, 0xFF on failure.
- **HBF_HOST_READ (0xE3)** - Read one byte. Returns E = byte with A = 0, or A = 0xFF on EOF or error. In the browser build this call now pauses the guest until the file picker resolves, by rewinding PC and retrying until the file is provided or the picker is cancelled (new in v1.34; before that R8 saw an instant EOF and imported 0-byte files).
- **HBF_HOST_WRITE (0xE4)** - Write one byte. E = byte to write. Returns A = 0 on success, 0xFF on failure.
- **HBF_HOST_CLOSE (0xE5)** - Close host file. C = 0 closes the read file, C = 1 closes the write file. Returns A = 0 on success; closing the write file now returns A = 0xFF if the host-side flush failed, meaning the written file may be truncated, e.g. host disk full (new in v1.34 - `emu_host_file_close_write()` returns bool).
- **HBF_HOST_MODE (0xE6)** - Get/set transfer mode. C = 0 get (returns E = mode), C = 1 set (E = mode: 0=auto, 1=text, 2=binary). The mode is stored and reported but transfers are currently always raw bytes.
- **HBF_HOST_GETARG (0xE7)** - Get command-line argument by index. C = argument index (0 = first argument), DE = buffer address; the argument is copied null-terminated. Returns A = 0 on success, 0xFF if no such argument. Only useful if the embedding platform calls `setHostCmdLine()`; R8/W8 do not use it - R8 parses the CP/M command tail at 0080h itself, and W8 takes its filename from the CCP-parsed default FCB at 005Ch.
- **HBF_HOST_GETNAME (0xE8)** - Where the open write file will actually land, as text for the guest to print. C = buffer size at DE including the terminator, DE = buffer address. Returns A = 0 with a null-terminated string in the buffer; A = 0xFF and the buffer untouched when no write file is open or the backend has no answer, which is also what an emulator built before this existed returns from its unknown-function path. W8 treats a failure as "print the path that was asked for" rather than as an error, so a current `w8.com` still runs on an already-released front end. **Ports must make `emu_host_file_get_write_name()` answer with the effective destination** - the resolved/redirected path a native backend will really open, or the download/export name a sandboxed one will really use - because that string is now shown to the user. Note the range check in `getTrapTypeFromFunc()` widened from 0xE0-0xE7 to 0xE0-0xEF to route it.

### Where host files land per platform

- **CLI (Linux/macOS)** - Plain `fopen`. Bare filenames are relative to the directory the emulator was started from; absolute paths are used verbatim. CP/M's CCP uppercases the whole command tail before R8 sees it, so when an exact-case open fails the CLI retries the path case-insensitively (`resolve_path_case_insensitive` in `src/emu_io_cli.cc`, new in v1.34). W8 exports files with lowercase names, and the destination it reports is absolute and `realpath`-canonical so it names a place rather than a name.
- **WASM (browser)** - Opening for read triggers the browser file picker; the guest-supplied name is advisory only. Opening for write accumulates bytes in memory, and close triggers a browser download (`src/emu_io_wasm.cc`). A path is reduced to its last component with the shared `emu_host_path_basename()`; a name with separators in it is not a usable download filename.
- **Windows (z80cpmw port)** - Bare filenames are placed in the app's data folder; absolute paths (drive-letter, UNC, or rooted) are used verbatim (`resolveHostPath` in `emu_io_windows.cpp`). In MSIX/Store packaged builds the OS redirects the data folder into the package's `LocalCache` directory - which is exactly the case HBF_HOST_GETNAME exists for: that port already has `resolveRealPath()` and should return its result from `emu_host_file_get_write_name()`.
- **Sandboxed mobile (iOS/Android)** - There is no outer-OS path to honour at all. Reduce the requested path with `emu_host_path_basename()` and report the export location the file really reaches.

Footnote: each backend has a fallback name for a null write filename ("output.bin" CLI, "download.bin" WASM, "export.txt" Windows), but the path is effectively unreachable - the dispatcher always passes a string and W8 rejects a missing argument.

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

### Adding Files to Disk Images
Use cpmtools with the RomWBW diskdefs. `DISKDEFS` must point at a diskdefs file containing the `wbw_*` formats:

```bash
export DISKDEFS="$HOME/esrc/RomWBW-v3.5.1/Tools/cpmtools/diskdefs"

# Single-slice hd1k image
cpmrm -f wbw_hd1k disks/hd1k_utils.img 0:oldfile.com
cpmcp -f wbw_hd1k disks/hd1k_utils.img file1.com file2.com 0:

# Combo image: the RomWBW diskdefs include slice-offset variants
# (wbw_hd1k_0 = slice 0 after the 1MB prefix, wbw_hd1k_1, ...)
cpmcp -f wbw_hd1k_0 disks/hd1k_combo.img file1.com 0:
```

If your diskdefs file lacks the slice-offset variants, extract slice 0 (it starts at the 1MB mark, after the MBR prefix) with dd, patch it, and write it back in place:

```bash
dd if=disks/hd1k_combo.img of=slice0.img bs=1M skip=1 count=8
cpmcp -f wbw_hd1k slice0.img file1.com 0:
dd if=slice0.img of=disks/hd1k_combo.img bs=1M seek=1 conv=notrunc
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

Use cpmtools with the RomWBW diskdefs - see "Adding Files to Disk Images" above. For slice 0 of a combo image:

```bash
export DISKDEFS="$HOME/esrc/RomWBW-v3.5.1/Tools/cpmtools/diskdefs"
cpmcp -f wbw_hd1k_0 disks/hd1k_combo.img r8.com w8.com 0:
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
