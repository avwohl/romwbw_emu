# Changelog

All notable changes to **romwbw_emu** are documented here.

This file starts at `v1.35`. For most of this project's life the record of a
change has been the commit message that made it, and those messages are longer
and more specific than any changelog entry — they carry the measurements, the
counter-examples and the things that were deliberately *not* done. This file
summarises and points; `git log` is the detail. Open work is in
[`todo.txt`](todo.txt); what downstream ports have to do about a release is in
[`DOWNSTREAM.md`](DOWNSTREAM.md) and `docs/DOWNSTREAM_*.md`.

Downstream ports do not consume a *release* of this repository — `ioscpm`
symlinks into `src/`, `z80cpmw`'s vcxproj compiles it in place, and `cpmdroid`'s
CMakeLists pulls it from a sibling checkout — so a commit here reaches all three
on their next build, tag or no tag.

## [Unreleased]

`VERSION` reads **1.36**, bumped in `48f8c19`; there is no `v1.36` tag yet, so
everything below is unreleased. It follows the `v1.35` tag.

### Added

- **`W8` takes a host path, the way `R8` already does** — `W8 <cpmname>
  [hostpath]`. It used to read only the default FCB, so it could write exactly
  one place: a lowercased 8.3 name in the emulator's working directory. With no
  path it still does exactly that. The case handling is the interesting part:
  the CCP uppercases the whole command line, and `R8` recovers by retrying the
  path case-insensitively, which works because the file it wants exists. A write
  cannot — the last component is the file being created and will never match —
  so `resolve_write_path()` resolves the parent, which does have to exist, and
  lowercases the basename. On the web build a path cannot mean anything, so the
  basename becomes the suggested download name.
- **`--escape=none` really reserves nothing** (also `off`, `^@`), matching
  `z80cpmw`'s `keyboard.ctrlRToCpm`. It was previously a trap rather than a
  no-op: the literal branch took the first byte, so it quietly reserved the
  letter `n`. Both parse branches insist on an exact length now, which also
  rejects `--escape=non` and a multi-byte literal that used to truncate to its
  UTF-8 lead byte.
- **Console tests on a real pipe and a real pty.** `tests/cli_console.cc` —
  44 checks over 12 cases, each in a forked child so the statics in
  `emu_io_cli.cc` start clean. The pair that matters: a piped `"a\nb\n"`
  delivers two CRs, because a script's lines end with LF and `c_iflag` never
  applies to a pipe; on a tty Enter arrives as `0x0D` and Ctrl+J as `0x0A`,
  because `emu_io_init()` clears ICRNL and the rewrite has to be *absent* there
  or the two keys collapse into one. Either half alone still looks right in
  ordinary use, which is what makes it regress quietly, so both are checked and
  the tty pair is checked twice — once through the blocking read, once through
  the peek path.
- **A check that the disk-resident `R8`/`W8` match their source.**
  `disks/verify_disk_utils.sh` assembles each utility, extracts the copy each
  tracked image holds and compares, wired into `make -C src test` and skipping
  when `um80`, `ul80` or cpmtools are absent. Nothing builds these `.COM` files
  as part of any target — they were assembled once and copied into the images —
  which is exactly how `w8.com` in `hd1k_infocom.img` came to be stale.
- `tests/web_console_output.js` (12 checks, including a sweep of all 256 byte
  values) and `tests/vda_keyboard.cc` (15 checks).
- `HBDisk::total_sectors()`, named and placed to match
  `MemDiskState::total_sectors()` so the two kinds of disk read alike at the
  call sites.

### Fixed

- **`^E` was stolen twice over.** It is the `sim>` escape, and the blocking read
  both latched it *and* returned it to the guest, so one keypress moved the
  WordStar cursor (CP/M 2.2 reads `^E` as physical end of line) *and* froze the
  emulator. `emu_console_read_char()` returns the new `EMU_CONSOLE_RETRY` and
  `CIOIN` rewinds over the two-byte `OUT (0xEF),A` the way its non-blocking
  branch already did, so the guest gets no byte at all.
- **POSIX raw mode clears IXON**, so `^S` and `^Q` reach the guest instead of
  being XON/XOFF flow control — `^S` is WordStar cursor-left and `^Q` prefixes
  the whole second half of its command set.
- **`VDAKST` said "no key" however much was queued.** It set the pending count
  in `E` but left the status byte in `A` at zero, and `A` is what a caller
  tests, so a guest polling the video keyboard could never get past the status
  call. Its `CIO` twin, `CIOIST`, has always set both.
- **`VDAKRD` handed the guest a stale byte.** With no key pending it flagged the
  wait and returned *without rewinding PC*, so the Z80 proxy's `RET` fired
  immediately with `E` holding whatever the previous call left there: the guest
  took a stale byte for a keystroke and never came back for the real one.
  `VDAKRD` is now the same code as `CIOIN`, which has rewound since the
  non-blocking path was added. Both bugs are reachable from every port —
  `SYSGET_VDACNT` reports one VDA whatever the front end.
- **The MSVC build was broken by `v1.35`.** That release started measuring files
  in 64 bits and reached for `fseeko`/`ftello`; Visual C++ provides neither, so
  the Windows port stopped compiling the moment it took the release, while
  `docs/DOWNSTREAM_2026-08-07.md` told that port it "builds unchanged". The
  seek/tell pair joins `emu_sleep_ms` and `emu_strcasecmp` in `emu_io.h`, with
  `_fseeki64`/`_ftelli64` on MSVC. The offset type comes from the same place:
  MSVC's `off_t` is a 32-bit `long`, which would truncate past 2 GB — inside the
  range a combo disk image reaches.
- **A disk size was truncated silently.** Two sites computed `disk.size / 512`
  into a `uint32_t` from a `size_t`. A truncating conversion keeps the
  remainder, so an image just past 2 TiB would report as nearly *empty* rather
  than as huge, and `DIOCAP` would hand the guest a capacity with no relation to
  the disk. It clamps at `UINT32_MAX` now, which is the honest answer given
  HBIOS deals in 32-bit sector counts.
- **The web frontend starved xterm.js.** `Module.onConsoleOutput` passed CR, LF,
  BS, ESC and `0x20–0x7E` and dropped everything else — 157 of the 256 byte
  values, with BS altered rather than dropped. TAB never aligned a column, BEL never rang, FF never cleared, no byte
  with the high bit set arrived, and BS was rewritten `\b \b`, a *destructive*
  backspace, so a guest moving the cursor left erased the character it moved
  over. xterm.js is a more complete VT than any native front end in this family;
  this filter was what made it look otherwise. It forwards every byte now,
  keeping only the LF → CR LF rewrite.
- **`Module.onError` is implemented.** `emu_io_wasm.cc` has always called it and
  nothing ever defined it, so every error the core reported went nowhere — a
  large part of why the dead VDA/sound wiring survived unnoticed, since the one
  channel that would have complained was itself unplugged. That wiring is
  deliberately still not fixed; see `todo.txt`.
- **The web frontend forgot "Don't warn" across a ROM change.** `reloadDisks()`
  restored `is_manifest` but not `warning_suppressed`, and the ROM select's
  change handler never reaches the start path, which was the only other place
  the checkboxes were pushed into the core — so a user who ticked "Don't warn"
  and then picked a different ROM got the overwrite modal back with the checkbox
  still ticked in front of them.
- **`hd1k_infocom.img` carried a stale `w8.com`.** Its copy was byte-identical
  to combo's pre-fix one. Verified by running it, not by writing the file:
  booting combo with infocom as disk1 puts it on `G:`, and `W8 ZORK1.COM
  EXPDIR/ZORK.BIN` wrote `ExpDir/zork.bin`, 587 bytes. Mind the diskdef —
  `hd1k_combo.img` is `wbw_hd1k_0` because of its 1 MB MBR prefix, the plain
  8 MB `hd1k_infocom.img` is `wbw_hd1k`, and cpmtools with the wrong one prints
  a garbage directory rather than failing, which is how infocom was briefly and
  wrongly recorded as having no `w8.com` at all.
- The dead slice selects are removed from the web page.

### Changed

- `docs/DOWNSTREAM_2026-08-07.md` — the `v1.35` notice itself — carries a dated
  correction to its claim that `z80cpmw` "builds unchanged", when that port did
  not build at all. The port's sync commit was then made on a machine with no
  Windows toolchain, and that note is what made it look safe. The correction is
  in the notice rather than in `DOWNSTREAM.md`, because the notice is what a
  port re-reads when it takes the release.
- `verify_disk_utils.sh` reports `r8.com` as **not comparable** rather than as
  stale. Its first run produced a confident FAIL on r8 in both images, which was
  wrong: the two utilities were not built the same way. `w8.com` is a `ul80`
  memory image — 256 leading NOPs, code at file offset `0x100`, every address
  constant 256 higher — while `r8.com` is a bare `.COM` with code at offset 0.
  Both run, because CP/M loads either at `0100h` and the NOPs slide into the
  code, but a byte compare across the two layouts reports a mismatch for what
  may well be the same program. Whether `r8.asm` still corresponds to the
  shipped `r8.com` is genuinely unknown and is filed in `todo.txt` as such.

## [1.35] and earlier

Not written up here. See `git log` — the commit messages are the record.
