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
- `--diskN=<path>` - attach an image to slot N, `--disk0` through `--disk15`.
  Slot N is HBIOS unit N+2, so `--disk0` is the first hard disk. The unit is
  fixed; the drive letters are not - CBIOS gives each hard disk
  `max(2, 8 / number of hard disks)` slices, and a booted slice takes A:, so
  `--disk0` alone on a ROM boot is C: through J: and not C: D: E: F:. See
  `DISK_FORMATS.md`. The CLI help documented only 0 and 1 for a long time; the
  sixteen slots were always there.

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

Z80 sources: `src/r8.asm`, `src/w8.asm`. Neither has an `ORG`: M80 assembles each as one relocatable code segment and L80 bases a `.COM` at 0100h already, so an `org 0100h` in the source is applied on top of that and puts the code at 0200h behind 256 NOPs. `disks/verify_disk_utils.sh` assembles both and asserts that the `w8.com` it builds still carries the HBF_HOST_CAPS interlock - that half needs no network and no disk image, and `make -C src test` runs it. Given a directory of images, or by default a `romwbw-get` cache, it additionally checks the `r8.com` and `w8.com` on those images against what it just built. `disks/rebuild_disk_utils.sh` is gone with the tracked images: romwbw_disks builds the utilities into the images it publishes and asserts the same interlock there.

### HBIOS Functions Used

The utilities call custom HBIOS extension functions 0xE1-0xEA using the standard RomWBW calling convention (function code in B, invoked via `rst 8` in the guest). Source of truth: the enum in `src/hbios_dispatch.h` and the dispatch cases in `src/hbios_dispatch.cc`. The dispatcher does the actual host I/O through the `emu_host_file_*` platform API declared in `src/emu_io.h` (implemented per platform in `emu_io_cli.cc`, `emu_io_wasm.cc`, and downstream `emu_io_windows.cpp`).

- **HBF_HOST_OPEN_R (0xE1)** - Open host file for reading. DE = address of a null-terminated path string in guest memory. Returns A = 0 on success, 0xFF on failure.
- **HBF_HOST_OPEN_W (0xE2)** - Open host file for writing. DE = address of a null-terminated path string. Returns A = 0 on success, 0xFF on failure.
- **HBF_HOST_READ (0xE3)** - Read one byte. Returns E = byte with A = 0, or A = 0xFF on EOF or error. In the browser build this call now pauses the guest until the file picker resolves, by rewinding PC and retrying until the file is provided or the picker is cancelled (new in v1.34; before that R8 saw an instant EOF and imported 0-byte files).
- **HBF_HOST_WRITE (0xE4)** - Write one byte. E = byte to write. Returns A = 0 on success, 0xFF on failure.
- **HBF_HOST_CLOSE (0xE5)** - Close host file. C = 0 closes the read file, C = 1 closes the write file. Returns A = 0 on success; closing the write file now returns A = 0xFF if the host-side flush failed, meaning the written file may be truncated, e.g. host disk full (new in v1.34 - `emu_host_file_close_write()` returns bool).
- **HBF_HOST_MODE (0xE6)** - Get/set transfer mode. C = 0 get (returns E = mode), C = 1 set (E = mode: 0=auto, 1=text, 2=binary). The mode is stored and reported but transfers are currently always raw bytes.
- **HBF_HOST_GETARG (0xE7)** - Get command-line argument by index. C = argument index (0 = first argument), DE = buffer address; the argument is copied null-terminated. Returns A = 0 on success, 0xFF if no such argument. Only useful if the embedding platform calls `setHostCmdLine()`; R8/W8 do not use it - R8 parses the CP/M command tail at 0080h itself, and W8 takes its filename from the CCP-parsed default FCB at 005Ch.
- **HBF_HOST_GETNAME (0xE8)** - Where the open write file will actually land, as text for the guest to print. C = buffer size at DE including the terminator, DE = buffer address. Returns A = 0 with a null-terminated string in the buffer; A = 0xFF and the buffer untouched when no write file is open or the backend has no answer, which is also what an emulator built before this existed returns from its unknown-function path. W8 treats a failure as "print the path that was asked for" rather than as an error, so a current `w8.com` still runs on an already-released front end. **Ports must make `emu_host_file_get_write_name()` answer with the effective destination** - the resolved/redirected path a native backend will really open, or the download/export name a sandboxed one will really use - because that string is now shown to the user. Note the range check in `getTrapTypeFromFunc()` widened from 0xE0-0xE7 to 0xE0-0xEF to route it.
- **HBF_HOST_CAPS (0xE9)** - What this front end's host-file support guarantees. No inputs and no state, so a guest can ask it *before* opening anything, which is what makes it usable as a safety interlock. Returns A = 0 and E = capability bits (`EMU_HOST_CAP_SAFE_PATHS` = 0x01: a guest path is never used destructively); an emulator predating it answers A = 0xFF from the unknown-function path. W8 asks this before it will hand over a host path at all, and refuses if the answer is anything else. The value comes from `emu_host_path_caps()`, which each backend defines - the core deliberately does not, so a port that has not been updated fails to link rather than silently claim a guarantee its code does not make.
- **HBF_HOST_GETRNAME (0xEA)** - The read mirror of 0xE8: which file the open read is actually reading. Same convention - C = buffer size at DE including the terminator, DE = buffer address, A = 0 with a null-terminated string, A = 0xFF and the buffer untouched. R8 prints this as its `Reading:` line instead of the path the CCP shouted. Backed by `emu_host_file_get_read_name()`, which is a **required** backend function: `return "";` is a correct answer and R8 then falls back to printing the request. A backend whose read is a file picker should return `""` - the guest's string is a hint the user is free to ignore, so echoing it would be wrong rather than merely unhelpful, and the picked file's real name never reaches the core. That is what the browser build does.

### Where host files land per platform

- **CLI (Linux/macOS)** - Plain `fopen`. Bare filenames are relative to the directory the emulator was started from; absolute paths are used verbatim. CP/M's CCP uppercases the whole command tail before R8 sees it, so when an exact-case open fails the CLI retries the path case-insensitively (`resolve_path_case_insensitive` in `src/emu_io_cli.cc`, new in v1.34). W8 exports files with lowercase names, and the destination it reports is absolute and `realpath`-canonical so it names a place rather than a name.
- **WASM (browser)** - Opening for read triggers the browser file picker; the guest-supplied name is advisory only. Opening for write accumulates bytes in memory, and close triggers a browser download (`src/emu_io_wasm.cc`). A path is reduced to its last component with the shared `emu_host_path_basename()`; a name with separators in it is not a usable download filename.
- **Windows (z80cpmw port)** - Bare filenames are placed in the app's data folder; absolute paths (drive-letter, UNC, or rooted) are used verbatim (`resolveHostPath` in `emu_io_windows.cpp`). In MSIX/Store packaged builds the OS redirects the data folder into the package's `LocalCache` directory - which is exactly the case HBF_HOST_GETNAME exists for: that port already has `resolveRealPath()` and should return its result from `emu_host_file_get_write_name()`.
- **Sandboxed mobile (iOS/Android)** - There is no outer-OS path to honour at all. Reduce the requested path with `emu_host_path_basename()` and report the export location the file really reaches.

Footnote: each backend has a fallback name for a null write filename ("output.bin" CLI, "download.bin" WASM, "export.txt" Windows), but the path is effectively unreachable - the dispatcher always passes a string and W8 rejects a missing argument.

## Disk Images

### Recommended Disk
**hd1k_combo** (51,380,224 bytes) - the catalog gives it `defaultSlot` 0, so
`@disk0` resolves to it. Six slices, as the catalog's own description of
`hd1k_combo` gives them - `romwbw-get list --disks` prints it verbatim, so that
is the copy to trust rather than this one:
- Slice 0: CP/M 2.2, with R8.COM and W8.COM for host file transfer
- Slice 1: ZSDOS
- Slice 2: NZCOM
- Slice 3: CP/M 3 - booting it headless prints `CP/M v3.0 [BANKED] for HBIOS`
- Slice 4: ZPM3
- Slice 5: word processing

### Where it comes from

This repository ships no disk image, and has not since v1.40. `hd1k_combo` and
the two dozen others are published by
[avwohl/romwbw_disks](https://github.com/avwohl/romwbw_disks) and fetched by
`tools/romwbw-get`, which checks every byte against the catalog's SHA-256.
[CATALOG.md](CATALOG.md) is the account of it.

```bash
romwbw-get list --disks          # what a release publishes
romwbw-get path @disk0           # the verified original, mode 0444
romwbw-get path --work @disk0    # a writable copy - what the emulator opens
```

The two are different files on purpose: `hbios_dispatch.cc` opens every image
`"rw"`, so the first CP/M `SAVE` into the cached copy would break the hash it
was verified against. Any port with a download cache needs the same split.

`~/www/romwbw` and `~/www/romwbw1` are still the production and development web
trees, but they are populated by `romwbw-get mirror` as part of `make -C web
deploy-*`, not copied from a working tree.

**What a port should take from this is not a path.** A native client should read
the same catalog rather than embed an image or a download URL: `index-v0.json`
-> `catalog-v0-<release>.json` -> the assets, hash-checked at each step, so that
a newly published disk reaches an already-installed app with no release. All
three GUI clients moved to it on 2026-09-05, and `tools/romwbw-get` is a working
reference implementation in 1300 lines of standard-library Python.

### Adding Files to Disk Images

Use cpmtools with this repository's `disks/diskdefs`, which carries `wbw_hd1k`
and the per-slice `wbw_hd1k_0` through `wbw_hd1k_5`. Run cpmtools from `disks/`:
cpmtools reads `./diskdefs` if there is one and the system copy otherwise, one
or the other and never both, so the working directory is the whole of the setup.

**Do not set `DISKDEFS`.** A path that does not exist is silently ignored, and
no distribution's system copy can be assumed to have the slice definitions -
Debian's and Ubuntu's cpmtools 2.23 has `wbw_hd1k` and no `wbw_hd1k_0` at all.
[DISK_FORMATS.md](DISK_FORMATS.md) has the detail.

```bash
cd disks

# a plain 8 MB image
cpmcp -T logical -f wbw_hd1k "$(../tools/romwbw-get path --work hd1k_cpm22)" file1.com file2.com 0:

# a combo image: wbw_hd1k_0 is slice 0, past the 1 MB MBR prefix
cpmcp -T logical -f wbw_hd1k_0 "$(../tools/romwbw-get path --work @disk0)" file1.com 0:
```

Do not drop the `-T logical`. cpmtools 2.23 cannot be configured without libdsk,
and its default libdsk path double-counts `boottrk` on a diskdef that carries no
`offset` - which `wbw_hd1k` is - so the bare form writes one boot area too far
into the file, exits 0, and puts the file's data over a block that was already
in use. `-T logical` bypasses the auto-probe and it lands where the geometry
says. The cause and the measured byte offsets are in
[DISK_FORMATS.md](DISK_FORMATS.md#always-pass--t-logical).

The dd extract-patch-write-back this section used to prescribe is not needed
for slice 0. `wbw_hd1k_0` accounts for the 1 MB prefix itself, and the case it
was written for - a diskdefs file without the slice variants - is answered by
running from `disks/` rather than by cutting the image apart. Slices 1 to 5 are
the exception: they sit past libdsk's 8 MB addressing ceiling, `-T logical` does
not lift it, and cutting the slice out is still the way in
([DISK_FORMATS.md](DISK_FORMATS.md#reaching-slices-1-to-5)).

## GitHub Release (avwohl/ioscpm) - history, superseded 2026-09-05

**Nothing in this section is current.** It describes the distribution channel
the GUI clients used until 2026-09-05, when all three moved to romwbw_disks'
`index-v0.json`. It is kept because the URLs in it are still live and have to
stay that way.

Until then the iOS/macOS app fetched disk images from an `ioscpm` release,
through an XML catalog:
- Catalog URL: `https://github.com/avwohl/ioscpm/releases/latest/download/disks.xml`
- Disk base URL: `https://github.com/avwohl/ioscpm/releases/latest/download/`

The v1.1 release assets:

| File | Size | Contains R8/W8 |
|------|------|----------------|
| disks.xml | 1,431 bytes | N/A (catalog) |
| hd1k_combo.img | 51,380,224 bytes | Yes |
| hd1k_utils.img | 8,388,608 bytes | Yes |
| hd1k_cpm22.img | 8,388,608 bytes | No |
| hd1k_zpm3.img | 8,388,608 bytes | No |
| hd1k_zsdos.img | 8,388,608 bytes | No |

### Those tags stay live indefinitely

Shipped builds are hardwired to those asset URLs and have no way of being told
otherwise. Deleting a tag or an asset breaks a copy of the app that is already
on somebody's phone, and there is no server-side redirect to soften it. Do not
tidy them up, and do not renumber them.

Equally, do not publish into them. `gh release upload v1.1 --clobber ...`, which
this section used to give as the update procedure, would now change what an old
build downloads while changing nothing for any current one - a silent divergence
between two populations of users, which is the opposite of what a `--clobber`
was ever for.

### Where a port should look instead

The catalog described in [CATALOG.md](CATALOG.md):
`index-v0.json` -> `catalog-v0-<release>.json` -> the assets, each step checked
against a hash named by the step above it. New disks appear there without any
client release, which is the entire reason the move happened.

## Reference Files

- `src/romwbw_emu.cc` - Main emulator with HBIOS handlers
- `src/hbios_dispatch.cc` - HBIOS function dispatch
- `src/hbios_cpu.cc` - I/O port emulation
- `src/emu_io.h` - Platform I/O interface
- `src/emu_io_common.cc` - Shared portable I/O implementation
- `src/emu_init.cc` - Shared initialization
- `docs/DISK_FORMATS.md` - Detailed disk format documentation
- `src/r8.asm`, `src/w8.asm` - Z80 source for file transfer utilities
