# Downstream Update Notice - 2026-08-07 (core v1.34 -> v1.35)

Addressed to every port that compiles this core: the Windows port
(`z80cpmw`), iOS/macOS (`ioscpm`), Android (`cpmdroid`), and the web/WASM
frontend in this repo. The canonical integration contract remains
[../DOWNSTREAM.md](../DOWNSTREAM.md); this file is the dated to-do list for
this sync.

v1.35 is a correctness and reproducibility release. Your `emu_io_*.cc` needs
no changes - unlike v1.34, there is no platform API break. But there **is
one new header your build must be able to find** (section 1), and several
failures that used to be silent now say what is wrong, one of which changes
a return value your UI should react to.

## 1. Required: add `src/romwbw_pin.h` to your tree

`hbios_dispatch.cc`, `emu_init.cc` and `romwbw_emu.cc` now include
`romwbw_pin.h`. Any port that references core files individually rather than
adding the whole `src/` directory will fail to compile with:

```
fatal error: 'romwbw_pin.h' file not found
```

- **ioscpm** symlinks each core file into `iOSCPM/Core/`, so a quoted include
  resolves against `Core/`, not against this repo, and the new header is not
  there. Add one more link:
  `ln -s ../../../romwbw_emu/src/romwbw_pin.h iOSCPM/Core/romwbw_pin.h`
  (done in this sync; verified with an `xcodebuild -destination
  'generic/platform=iOS'` build).
- **cpmdroid** needs the equivalent copy or symlink next to its other core
  sources.
- **z80cpmw** compiles the core files in place from
  `$(SolutionDir)..\romwbw_emu\src\`, so the include already resolves and it
  builds unchanged. It was still added to `z80cpmw.vcxproj` as a `ClInclude`
  for IDE visibility, alongside the other core headers.

It is a header only, no build-phase membership required; it just has to be
resolvable from wherever your build sees `hbios_dispatch.cc`.

## 2. Required: handle a failed ROM load

`emu_load_rom()` and `emu_load_rom_from_buffer()` now validate the HBIOS
configuration block (HCB) in bank 0 and **return false** for a ROM this core
cannot run. Previously they accepted anything, and a bad ROM produced an
emulator that started its CPU and printed nothing at all - no dialog, no log
line, no boot menu.

They fail when:

- the HCB marker at 0x103 is not `57 A8` (corrupt image, or not a RomWBW ROM)
- the HCB version bytes do not match the pinned RomWBW release

They warn but still load when `CB_PLATFORM != 0`, which means a stock RomWBW
ROM built for real hardware rather than an `emu_*.rom` with the port 0xEF
proxy. Such a ROM may reach the boot loader and then misbehave.

**What to do:** you almost certainly already treat `false` as fatal - check
that you *surface* it. A GUI port loading a ROM from its bundle or from a
download is the case this protects, and it is also the case with no console
to notice the old silent failure on. `emu_validate_rom_hcb(rom, size)` is
public (`emu_init.h`) if you want to screen an image before offering it in a
picker; it returns `nullptr` when the ROM is usable, otherwise a message
suitable for showing to the user.

## 3. The RomWBW version pin

The RomWBW release this core emulates now has one home,
`src/romwbw_pin.h`, instead of being repeated across C++, assembly, scripts
and prose. `HBF_SYSVER`, the NVRAM checksum seed, the HCB in
`src/emu_hbios.asm`, the build script and the docs all derive from it. It is
still **RomWBW v3.5.1** - this release changes where the number lives, not
what it is.

Why you should care: your ROM, your disk images and this core have to come
from the same release. A boot slice built by a different RomWBW prints
`*** WARNING: HBIOS/CBIOS Version Mismatch ***` at best, and at worst the
ROM never reaches the boot loader.

**What to do:**

- Run `roms/verify_romwbw_pin.sh` against your bundled ROM and disk images -
  in CI if you have it, otherwise before cutting a build. It re-derives the
  expected bytes from the pin and checks every ROM (marker, version,
  platform), every disk image (the `CBIOS v<pin> [WBW]` string in its boot
  slices) and the built binary. Exit 0 means consistent; exit 1 names every
  mismatch. Point it at your own tree with an argument.
- If your About screen shows a version, add the pin. The CLI now prints
  `RomWBW compatibility: v3.5.1 (pinned)`; `ROMWBW_PIN_STR` is available to
  anything that includes `romwbw_pin.h`.
- Keep pinning disk-catalog downloads to a specific release tag, as the
  Windows and iOS ports already do. A "latest" catalog can hand a user disk
  images from a RomWBW your build does not emulate.

## 4. Free fixes you inherit by recompiling

`emu_io_common.cc` is shared, so ports that compile it (iOS and Android via
symlink, the CLI and WASM frontends here) get these on rebuild. The Windows
port carries its own copy and already fixed the equivalents in its commit
79ddfc4; this is the same audit applied upstream.

- **`emu_file_load()` no longer crashes on an unseekable path.** It used to
  do `size_t size = ftell(f)`, turning ftell's `-1` into `SIZE_MAX` and
  passing that to `vector::resize`, which throws `length_error` - with no
  handler anywhere in the emulator, that terminates the process. Any path
  naming a pipe, socket or similar opens fine and is not seekable, which on
  mobile means a document handed over by a file picker. Now measured in 64
  bits, checked, and capped at 2 GiB.
- **`emu_file_save()` is atomic.** It opened the target with `"wb"`, which
  truncates before the first byte is written: a disk-full error or a kill
  mid-write destroyed the previous disk image and left nothing in its place.
  It now writes `<path>.tmp` and renames over the target, and checks
  `fclose` (stdio can surface a write error only at the final flush). If
  your port persists in-memory disks through this function - the Windows and
  Android ports do - this is the difference between a failed save and a lost
  disk.
- **`emu_disk_read()`/`emu_disk_write()` check the seek.** An unchecked
  `fseek` leaves the stream where it was, so a failure read or wrote some
  other part of the image and reported success - overwriting the MBR of a
  combo image, for instance. Both now use `fseeko` and return 0 on failure,
  which `DIOREAD` treats as end-of-media and `DIOWRITE` reports as `HBR_IO`.
- **`emu_disk_open()` fails instead of recording `SIZE_MAX`.** The recorded
  size drives format auto-detection and slice math.
- **`emu_validate_disk_image()`** reports "not a regular file (cannot
  determine size)" instead of quoting a 16-exabyte size in a generic
  "invalid disk size" message.

**What to do:** rebuild. If your port keeps its own copy of any of these
(the Windows port does), apply the same guards - `git show <this release>`
in `src/emu_io_common.cc` is the reference.

## 5. Every `emu_*.rom` was rebuilt - re-bundle them

**Action required if you ship a ROM.** All three emulator ROMs
(`emu_avw.rom`, `emu_romwbw.rom`, `emu_rcz80.rom`) have been regenerated
from `src/emu_hbios.asm` with um80 0.3.44 and now reproduce from source.
Their bytes changed, so a client bundling an older copy is shipping a ROM
that no longer matches this tree. Take the new ones.

The change is confined to bank 0 (our HBIOS proxy): 98 bytes differ from the
previously shipped `emu_avw.rom`, all below 0x7B2, and banks 1-15 - romldr,
the OS images and the ROM disk, all from RomWBW v3.5.1 - are byte-identical.
The shipped ROMs had been built from an assembly older than the checked-in
source, so what you are getting is the source you can actually read.

Verified after the rebuild: all three boot their loaders, report
`CBIOS v3.5.1 [WBW]` with **no** HBIOS/CBIOS mismatch warning, boot CP/M
2.2 from a combo disk, and write files that survive a restart. NVRAM
round-trips too (`NV Switches Found`), which exercises the checksum seed the
pin now feeds.

Three files in `roms/` were also traps, and the new validation is what
surfaced them. If your build script copies ROMs out of this directory by
wildcard, check what you are shipping:

- `emu_rcz80.rom` was built from a stale `emu_hbios.bin` whose HCB marker
  had one corrupted bit (`0xB8` where `~'W' = 0xA8` belongs). It started and
  produced no output. It now boots RCZ80 CP/M 2.2 with a matching CBIOS, as
  `docs/ARCHITECTURE.md` always claimed it did.
- `emu_hbios.bin` was a tracked build intermediate that had gone stale in
  December 2025 - the source of the corruption above. **Removed and
  gitignored**; `roms/build_from_source.sh` regenerates it.
- `SBC_simh_std_v360.rom` is a stock RomWBW **v3.6.0** ROM. Overlaying bank
  0 onto it produces banks 1-15 from a release this core does not emulate.
  **Moved to `archive/romwbw-v3.6.0/`**, where it stays available for the
  eventual upgrade.

`SBC_simh_std.rom` and `RCZ80_std.rom` remain in `roms/`: they are stock
ROMs, not runnable here, but they are the build inputs
`roms/build_emu_rom.sh` overlays. The verifier flags them as warnings so
nobody points `--romwbw` at one by mistake.

## 6. `roms/build_from_source.sh` produced a dead ROM from the wrong cwd

The script read `emu_hbios_32k.bin` by bare name while writing everything
else through `$SCRIPT_DIR`. Run from anywhere but `roms/`, bank 0 stayed all
zeros and you got a plausible 512KB ROM that dies at startup - the failure
was hidden because dd's stderr went to `/dev/null`. Fixed, dd errors are
visible, and the script now verifies the HCB of what it just built and
deletes the output rather than leave a broken image behind.

It also falls back to the copy of the pinned release's ROM in `roms/`, so a
fresh clone can rebuild the ROM offline instead of downloading RomWBW's
199MB `Package.zip`.

## 7. Note for ports that symlink this repo

`ioscpm` reaches this core through symlinks of the form
`../../../romwbw_emu/src/...`, which resolve only when `romwbw_emu` and
`ioscpm` are siblings. If the core files show as missing in your project,
that is a checkout-layout problem, not a missing file - put the two
checkouts side by side (a symlink to the real checkout is enough).

## Migration checklist

- [ ] Add `romwbw_pin.h` to your tree (symlink/copy next to the other core files)
- [ ] Rebuild against v1.35
- [ ] Surface a `false` return from `emu_load_rom*()` to the user
- [ ] Run `roms/verify_romwbw_pin.sh` over the ROM and disks you ship
- [ ] Add the RomWBW pin to your About screen if it shows a version
- [ ] If you keep private copies of the `emu_io_common.cc` helpers, apply the
      hardening in section 4
- [ ] Re-bundle the rebuilt `emu_*.rom` (their bytes changed this release)
- [ ] Re-check any wildcard copy of `roms/*.rom` in your packaging
