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

## [1.36] - 2026-08-25

`VERSION` was bumped in `48f8c19` and tagged here. It follows `v1.35`.

Downstream ports do not need this tag — they build these sources out of a
sibling working tree, so they had the core changes at the commit. What the tag
is *for* is this repository's own users: the deb/rpm CLI binary, and the web
build, whose wasm nothing in this tree can produce (emcc is not a build
dependency here). The browser half of this release — the shared basename
reduction, the zero-byte download, a refused disk reported as refused, and
`W8`'s whole `To host:` change — reaches a web user only when CI builds it.

The disk images are a separate channel again and are **not** in this release:
`release.yml` stages the binary, the web page and `roms/` only. The refreshed
`r8.com`/`w8.com` reach the mobile and Windows ports through `ioscpm`'s release
assets, which all three catalogs point at — and the ordering constraint in
[docs/DOWNSTREAM_2026-08-25.md](docs/DOWNSTREAM_2026-08-25.md) applies to that
refresh: the sanitiser fix has to land in `ioscpm` before, or with, the new
images.

### Added

- **`W8` says where the file actually went** — new HBIOS extension function
  `HBF_HOST_GETNAME` (0xE8): `C` = buffer size at `DE`, `A` = 0 and the buffer
  holds the effective destination. It used to echo the path the user typed,
  which is never the path that gets written. The CCP uppercases the whole
  command line, so the emulator has to resolve the directory case-insensitively
  and lowercase the file name; and on the browser and the sandboxed mobile
  ports there is no outer-OS path to honour at all, so the file lands in a
  download folder or the app's own Exports area under a name the guest never
  sees. `To host: /HOME/ME/OUT.TXT` named nothing on three of the five front
  ends. On the CLI the answer is now absolute and `realpath`-canonical, so it
  names a place rather than a name. A failure is not an error: an emulator
  built before 0xE8 existed answers "no such function" and `W8` falls back to
  printing what was asked for, so a current `w8.com` still runs on an
  already-released front end. `getTrapTypeFromFunc()` widened its extension
  range from 0xE0-0xE7 to 0xE0-0xEF to route it.
- **`emu_host_path_basename()`** in `emu_io_common.cc`, for the front ends with
  no filesystem to honour a directory with. Takes **both** separators, because
  the string comes off a guest command line that may have been typed on any
  host, and never returns `""`, `"."`, `".."` or a bare drive letter. There
  were three different answers before: the browser split on `/` only, and the
  iOS backend did not split at all.
- **`R8` and `W8` take the whole rest of the command line as the path.** Both
  stopped at the first space, and `/Users/me/My Documents` and
  `C:\Program Files` are ordinary paths on two of the five hosts. Trailing
  spaces are trimmed.
- **`emu_rename()`** in `emu_io.h`, beside the `emu_fseek`/`emu_ftell` pair.
  ISO C leaves `rename()` undefined when the target exists and both the MSVC
  CRT and mingw's msvcrt refuse it outright, so `emu_file_save()` — which
  renames a temp file over the image precisely so a failed write cannot destroy
  the previous one — had the safe path be the broken one on Windows. Asked for
  by `z80cpmw`, which had already worked around it locally.
- **CI runs the tests.** `.github/workflows/test.yml`: build, `make -C src
  test`, and then an assertion that the disk check did not silently skip. There
  was no test step anywhere before; `release.yml` builds and packages only.
- `tests/hbios_hostname.cc` (29 checks: the `HBF_HOST_GETNAME` buffer bound,
  the not-writing and empty-name answers, and both ends of the widened function
  range) and 20 more checks in `tests/cli_hostfile.cc`.
- `disks/rebuild_disk_utils.sh` — assembles both utilities and installs them
  into every tracked image. The recipe existed only as prose, in three places
  that had already drifted from each other, and it did not work as written:
  `cpmcp` will not overwrite, so the old copy has to be removed first.

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

- **`W8` truncated every binary export, silently.** It stopped at the first
  `1Ah` byte. `1Ah` is `LD A,(DE)` and appears in almost any `.COM` file:
  exporting `W8.COM` itself produced **368 bytes of 1408**, reported as
  `Done: 368 bytes`. It now copies the file whole and drops only the run of
  `1Ah` at the very end — the padding CP/M writes into a file's last record,
  and exactly what `R8` puts there on import. So a text file imported with `R8`
  still comes back byte for byte, and a binary containing `1Ah` no longer
  loses everything after the first one. A file whose real content ends in `1Ah`
  loses that tail; CP/M stores no length, only whole 128-byte records, so
  nothing can tell the two apart. That is the documented boundary.
- **`R8` erased unrelated CP/M files.** It copied the host basename into the
  FCB unfiltered, and `?` and `*` make an FCB *ambiguous* — then handed that
  FCB to `F_DELETE` before `F_MAKE`. Importing a host file called `a?b.txt` did
  not create one CP/M file, it deleted every file matching `A?B.TXT` first, and
  said nothing. Verified live: two unrelated files went with it. Illegal
  characters now become `-` and `R8` says when it substituted.
- **A host name containing `_` imported as a file nothing could reach.**
  Underscore is in the CP/M 2.2 CCP's delimiter set, which is not obvious —
  `my_file.txt` imported as `MY_FILE.TXT`, and then `DIR MY_FILE.TXT` said
  `NO FILE` while `DIR MY?FILE.TXT` listed it, `ERA` could not remove it,
  `TYPE` printed `MY_FILE.TXT?`, and `W8` could not export it because the CCP
  parsed the argument as `MY`. The data was on the disk and unreachable, and a
  directory slot was consumed permanently. `_` is rejected along with the rest
  of the delimiter set now, and the substitute is `-`, which is not one — the
  first version of this fix substituted `_` and made the problem worse.
- **`R8` destroyed a CP/M file when handed a host directory.** `fopen` on a
  directory succeeds on Linux and macOS — only the first read fails — so the
  open reported success, `F_DELETE` and `F_MAKE` ran, and the guest was left
  with an empty file and `Done: 0 bytes`, which is what a legitimately empty
  import also prints. The CLI backend refuses a directory now, before the open
  can report success.
- **`R8` announced a CP/M file it had not created**, printing `Creating: NAME`
  before opening the host file.
- **`R8` made CP/M files nothing could address.** Two ways. A path ending in a
  separator reached `F_MAKE` with the FCB's eleven blanks intact and created a
  nameless directory entry; and the extension copy stopped only at the NUL, so
  `a.b.c` produced a file named `A` with type `B.C`, which the CCP's own parser
  cannot name because it reads the first dot as the delimiter. The type now
  comes from the last dot and the name stops at the first, so
  `archive.tar.gz` is `ARCHIVE.GZ` and `notes.2024.txt` is `NOTES.TXT`; a
  leading dot is part of the name, so `.profile` is `PROFILE`; and a path that
  names no file is refused before anything is opened or deleted.
- **`R8` reported success for a short import.** Every full record write was
  checked and the final partial one was not, so a CP/M disk that filled on the
  last record printed `Done`. `F_CLOSE`'s status was discarded too, and
  `F_CLOSE` is where CP/M writes the directory entry back.
- **The byte count `R8` and `W8` print was the low 16 bits**, so a
  100000-byte transfer reported `34464`.
- **The `ORG` came out of both utilities.** `org 0100h` was wrong for this
  toolchain: M80 assembles each as one relocatable code segment and L80 bases a
  `.COM` at 0100h *by itself*, so the ORG was applied on top of that base and
  pushed the code to 0200h behind 256 zero bytes. It ran only because CP/M
  loads the whole file at 0100h and the Z80 slides through the NOPs. `w8.com`
  was built that way and `r8.com` was not, which is the only reason
  `verify_disk_utils.sh` could not check both.
- **`verify_disk_utils.sh` exited 0 after verifying nothing**, and `make test`
  called that a pass. A wrong diskdef, a renamed image or a deleted source all
  land there — and a wrong diskdef is not loud, cpmtools prints a garbage
  directory rather than failing. An image *missing* a utility was an info line
  and a pass too, which is precisely the shape the stale `w8.com` incident
  took; it is a failure now, and CI asserts the count rather than the verdict.
- **`HBF_HOST_GETNAME` could report a path that names something else.** `C` is
  one byte, so a destination longer than 254 characters did not fit — and a
  bare name gets the whole working directory prepended, so a deep checkout
  reaches that. Clamping handed back a path chopped mid-component that `W8`
  then printed as fact, which is the failure the call exists to remove. It
  keeps the *end* of the path now, where the file name is, behind a leading
  `...` that makes the answer read as a fragment.
- **`HBF_HOST_GETARG` wrote its terminator outside the buffer** for an argument
  longer than 255 characters: the copy was clamped and the NUL was not, so the
  guest got an unterminated buffer and a zero byte dropped past the end of it.
  Unreachable here (nothing calls `setHostCmdLine`), reachable from any port
  that wires it up.
- **The browser dropped a zero-byte export** and kept the CCP's uppercase in
  the download name while the CLI lowercased it. An empty CP/M file is a real
  file, and the same `W8` command should not produce differently-named files on
  different front ends.
- **The web page reported "Disk N loaded" for a disk the core refused.**
  `loadDiskData()` discarded `_romwbw_load_disk`'s return value, so a stray
  small file dropped into the slot looked like a successful load with the only
  trace in the JS console — and `reloadDisks()` would push it back in on the
  next ROM change.
- **The shared core accepted a 0-byte disk image** and mounted it as an empty
  disk. `z80cpmw` reported the same hole on its side and asked for it to be
  closed in both.
- **`emu_file_load_to_mem()` still used the bare 32-bit `fseek`/`ftell` pair**
  the rest of that file was hardened away from in v1.35 — the last site.
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
- `verify_disk_utils.sh` reported `r8.com` as **not comparable** rather than as
  stale, because the two utilities were not built the same way. That is settled
  now — see "The `ORG` came out of both utilities" below — and the question it
  left open is answered: `r8.asm` **does** still correspond to the shipped
  `r8.com`, byte for byte in both images. The check covers all four copies.

## [1.35] and earlier

Not written up here. See `git log` — the commit messages are the record.
