# Downstream Update Notice - 2026-07-21 (core v1.33 -> v1.34)

Addressed to the iOS and Android ports of this emulator core. Core v1.34
(commit 898e175) imports the useful work from the z80cpmw Windows port
(its v1.0.13 through v1.0.17) and fixes real bugs found by sweeping this
codebase with the audit patterns from that port's crash investigations.
This notice tells you exactly what to change when you pull the new core.
The canonical integration contract remains ../DOWNSTREAM.md; this file is
the dated to-do list for this sync.

## 1. Required: your build breaks until you do these

The v1.34 core changes the emu_io platform API. Your platform layer
(emu_io_ios.*, emu_io_android.*, or equivalent) must be updated:

- `emu_host_file_close_write()` now returns `bool` instead of `void`.
  Return false if the final flush/close of the exported file failed (disk
  full, I/O error) - the guest's W8 utility now reports "file may be
  truncated" to the CP/M user when you do. If your port buffers the write
  and hands it to the OS asynchronously (share sheet, SAF document), return
  true, the same as the browser backend does.
- New required function `bool emu_console_input_exhausted();` - return
  `false`. It only means something for a CLI reading a pipe.
- New required function `bool emu_console_input_eof();` - return `false`.
  Same reason.

Both functions exist so the CLI can exit cleanly (saving NVRAM and trace
output) when its piped stdin ends; interactive platforms never exhaust
input. They must still be defined or the link fails.

## 2. Free fixes you inherit by recompiling

No action needed beyond rebuilding against the new core, but you should
know what changed underneath you:

- Disk writes that fail on the host (disk full, EIO) now return HBIOS
  error `HBR_IO` to the guest with a partial block count, instead of
  reporting success and silently losing data. RomWBW's CBIOS surfaces this
  as "Bdos Err ... Bad Sector" - that is the fix working, not a regression.
- Guest writes past the end of an in-memory disk image now return `HBR_IO`
  instead of silently growing the image. If your port ever depended on an
  image growing on write (it should not have - the image size drives
  format auto-detection), that behavior is gone.
- All disk byte offsets are computed in 64-bit. Previously a guest seek to
  a large LBA could wrap the 32-bit `lba * 512` multiply and read or write
  the wrong part of the image - including overwriting the MBR of a combo
  image.
- `emu_load_romldr_rom()` rejects files larger than 512KB (previously a
  heap overflow) and short reads.
- `HBF_HOST_CLOSE` propagates the new close-write result to the guest.
- `HBIOSDispatch::closeDisk()` now clears the per-unit dirty flag, so a
  freshly loaded image starts clean. If you use `isDiskDirty()` to decide
  when to warn or persist, stale dirty state from a previous image is gone.

## 3. Behavioral change to verify: host file transfer (R8) with a picker

`HBF_HOST_READ` now pauses the guest while the host-file state is
`HOST_FILE_WAITING_READ` and blocking is disallowed
(`setBlockingAllowed(false)`, which non-blocking GUI run loops use). The
PC-rewind mechanism is the same one CIOIN uses, and
`isWaitingForInput()` now reports true during this wait.

Before v1.34, a port that showed a file picker for R8 had a race: the
guest saw instant EOF and imported a 0-byte file (this was deterministic
in the browser build). Now the guest waits. What you must verify:

- Your run loop already yields when `isWaitingForInput()` is true (it
  does if you followed DOWNSTREAM.md). Nothing new needed there.
- Every path out of your picker must resolve the wait: deliver bytes via
  `emu_host_file_provide_data()` (state moves to READING), or cancel so
  the state returns to IDLE (the guest then sees EOF and R8 aborts
  cleanly). If your picker can be dismissed without a callback (iOS
  `documentPickerWasCancelled`, Android `RESULT_CANCELED` or the user
  backing out), wire that to the cancel path - otherwise the guest now
  waits forever where it previously imported an empty file.

While you are in that code, check the path contract that bit the Windows
port (its commit 89a4f28): never unconditionally prepend your sandbox/data
directory to the filename the guest passes. Bare names may resolve to your
app's documents directory; an absolute path must be used verbatim. The
contract is now spelled out above `emu_host_file_open_read()` in
src/emu_io.h.

## 4. Check your tree for copied helpers

The Windows port carried a verbatim copy of `emu_file_load_to_mem()` in
its platform file and inherited a size_t underflow from it (offset past
the buffer wraps the subtraction and reads out of bounds). The upstream
copy is fixed in v1.34. Search your port for a private copy of this
function (or any `mem_size - offset` arithmetic copied from old emu_io
code) and apply the same guard: return early when `offset >= mem_size`
before subtracting.

## 5. Ship updated disk images (w8.com was broken)

If your app bundles or downloads hd1k disk images containing the R8/W8
transfer utilities, the w8.com on every image built before 2026-07-21 has
a broken lowercase conversion: um80 0.3.42 miscompiles `add a,'a'-'A'` to
`add a,0`, so W8 exported UPPERCASE filenames. The fixed w8.com is
embedded in this repo's refreshed `disks/hd1k_combo.img` (slice 0) and
`disks/hd1k_infocom.img` - take those, or refresh your own images with
cpmtools (`cpmrm` + `cpmcp -f wbw_hd1k image.img w8.com 0:w8.com`).

To audit an image, hex-scan it for the tolower byte sequence:
`fe41d8fe5bd0c600` is the broken build, `fe41d8fe5bd0c620` is fixed.

Also pin any disk-image downloads to the RomWBW v3.5.1 release: the
core's built-in HBIOS identifies as v3.5.1, and boot slices from other
RomWBW releases print a HBIOS/CBIOS version-mismatch warning. (The
Windows port pins its disk-catalog tag for the same reason.)

## 6. Version string

The CLI now injects `EMU_VERSION` from the top-level VERSION file at build
time; the in-source fallback is "dev". If your build compiles files that
print the version, add the equivalent define (for example
`-DEMU_VERSION=\"1.34\"`, ideally generated from the VERSION file) or your
About screen will say "dev".

## 7. The lesson to adopt: flush dirty disks in your stop path

The one undocumented contract that actually bit the Windows port (its
commit 70ce7b1): platform stop/suspend paths must flush or persist any
dirty in-memory disk data, because the process can outlive the emulation
session. The core gives you everything needed - `isDiskDirty(unit)`,
`clearDiskDirty(unit)`, `flushAllDisks()`, and `emu_disk_flush_all()`.

- Android: wire persistence into `onPause`/`onStop` (the dirty-flag API
  was added for exactly this).
- iOS: do the same in `applicationDidEnterBackground` /
  `sceneDidEnterBackground`.
- Consider warning before app exit while any unit is dirty - v1.34's web
  frontend does this with a beforeunload prompt, cleared when the user
  downloads the disk.

## 8. Optional parity ideas from the Windows port

Nothing below is required; it is the UX work the Windows port did that
translates to mobile, cataloged in its FEATURE_PARITY.md
(https://github.com/avwohl/z80cpmw/blob/master/FEATURE_PARITY.md):

- Settings persistence: the CLI gained a JSON settings file
  (docs/CONFIGURATION.md; schema: rom, boot, escape, debug, strictIo,
  symbols, romldr, disks[16], romapps). src/emu_config.cc is CLI-only -
  do not add it to your build - but reusing the schema keeps machine
  descriptions portable across the family. The web frontend's lighter
  answer (persist the UI selections; ROM choice, disk choices, slice
  counts, boot string, warning suppression) may fit mobile better.
- Configurable key mapping for special keys (the Windows port maps keys
  to termcap-style escape strings; useful if you support an external
  keyboard).
- Terminal scrollback, selection, and copy/paste.
- Crash reporting appropriate to the platform.

## 9. In-app help corrections

If your port shows help text derived from this repo's docs, sync these
corrections made today:

- Boot unit numbering: units 0 and 1 are the RAM and ROM memory disks and
  carry no OS - booting `0` prints "No system image on disk". The first
  attached hard disk is unit 2 (then 3, 4, ... in attach order). At the
  boot menu, `D` lists disk units, `L` lists ROM applications, `W` saves
  the choice as the autoboot default. Old examples using `--boot=0` for
  the first disk were wrong.
- R8/W8 usage: R8 imports a host file (picker on sandboxed platforms; the
  emulator now pauses until the pick resolves), W8 exports a CP/M file
  (with a lowercase name once your images carry the fixed w8.com).
- NVRAM boot settings persist as a single-line plain-text file (never
  nvram.json; the old docs were wrong). Where it lives is platform-defined;
  the CLI uses $XDG_CONFIG_HOME/romwbw_emu/nvram.

## Migration checklist

- [ ] Pull core v1.34 (commit 898e175) and rebuild
- [ ] Change `emu_host_file_close_write()` to return bool
- [ ] Add `emu_console_input_exhausted()` returning false
- [ ] Add `emu_console_input_eof()` returning false
- [ ] Verify every picker dismissal path cancels the host-file wait
- [ ] Verify absolute guest paths are used verbatim in host file open
- [ ] Grep for a copied `emu_file_load_to_mem` and guard the underflow
- [ ] Refresh bundled/downloaded disk images with the fixed w8.com
- [ ] Pin disk downloads to RomWBW v3.5.1
- [ ] Define EMU_VERSION in the build (or accept "dev")
- [ ] Flush/persist dirty disks on pause/stop/background
- [ ] Sync in-app help (boot units, R8/W8, NVRAM wording)
