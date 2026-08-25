# Downstream Update Notice - 2026-08-25 (core v1.36, unreleased)

Addressed to every port that compiles this core: the Windows port (`z80cpmw`),
iOS/macOS (`ioscpm`), Android (`cpmdroid`), and the web/WASM frontend in this
repo. The canonical integration contract remains
[../DOWNSTREAM.md](../DOWNSTREAM.md); this file is the dated to-do list for
this sync.

This one is about `W8.COM` and the outer-OS path it is given. **Read section 0
first if you maintain ioscpm** - the audit behind this release found a
data-destruction bug there, reproduced on a Mac, that this release's shared
helper is what closes.

Three defects in W8 itself, each of which had shipped for a long time:

1. **W8 truncated every binary export, silently.** It stopped at the first
   `1Ah` byte in the file. `1Ah` is `LD A,(DE)` and appears in almost any `.COM`
   file: exporting `W8.COM` itself produced **368 bytes of 1408**, reported as
   `Done: 368 bytes`. Measured, not theorised.
2. **W8 printed a destination that does not exist.** It echoed the path the
   user typed. The typed path is never what gets written: CP/M's CCP uppercases
   the whole command line, so the emulator has to resolve the directory
   case-insensitively and lowercase the file name - and on the browser and the
   sandboxed mobile ports there is no outer-OS path to honour at all, so the
   file lands in a download folder or the app's own Exports area under a name
   the guest never sees. `To host: /HOME/ME/OUT.TXT` named nothing on three of
   the five front ends.
3. **A host path could not contain a space.** Both utilities stopped at the
   first space in the tail. `/Users/me/My Documents` and `C:\Program Files` are
   ordinary paths on two of the five hosts.

There is **one new HBIOS function** (section 1), **one new shared helper** in
`emu_io.h` (section 2), and **one tightened contract** on a function you
already implement (section 3). Nothing is removed and nothing changes shape.

## 0. Urgent, ioscpm: `W8 ANYFILE.TXT ..` destroys the user's disk library

Found while auditing the path W8 now sends you, and reproduced on a Mac with
`swift`, not reasoned about. Both halves were measured:

- `emu_io_ios.mm:404-410` stores the guest string verbatim as the export name.
  Nothing between there and the Swift layer reduces it.
- `EmulatorViewModel.swift:1906-1908` does
  `exportsDir.appendingPathComponent(name)` and then
  `try? fm.removeItem(at: destURL)`. `appendingPathComponent` does **not**
  escape separators or dot components: `".."` yields `.../Exports/..`, and
  `removeItem` on that path succeeds and **recursively deletes the parent**. A
  test tree `Documents/{keepme.txt, Exports/a.txt}` was reduced to nothing.

`Documents` is where that port keeps `Disks`, `Imports` and `Exports` - every
downloaded disk image. So a CP/M program, or a mistyped command, running
`W8 ANYFILE.TXT ..` destroys the disk library, the `try?` swallows the error,
and the guest is told the export succeeded (`emu_host_file_close_write()`
returns true before Swift runs).

Do all three:

1. Reduce the guest string to one leaf component with the shared
   `emu_host_path_basename()` (section 2) inside
   `emu_host_file_open_write()`. That alone closes this, and closes it for
   every future UI layer too.
2. Assert `destURL.standardized.path` still has `exportsDir.standardized.path`
   as a prefix before writing.
3. Drop the `removeItem` - `Data.write(to:)` already replaces.

**Same shape on the read side**, lower severity but wrong answers rather than
lost data: `EmulatorViewModel.swift:2647-2659` builds
`importsDir.appendingPathComponent(filename)` from the guest's path, misses,
and then falls back to *the first file in the folder* and imports that instead.
`R8 /USERS/ME/DESKTOP/FOO.COM` puts unrelated contents into CP/M under the
requested name, and R8 in the guest prints its usual success line. Reduce to a
leaf name, look that up, and report a miss (`emu_host_file_cancel()`) rather
than substituting.

This is not new in this release - W8 has been able to send a path since
`98eb6a1`. It is in this notice because this is the release that gives you the
helper to fix it with.

## 1. New: HBF_HOST_GETNAME (0xE8) - where the export really went

```
  B  = 0xE8
  C  = size of the buffer at DE, including room for the terminator
  DE = buffer address in guest memory
  ->  A = 0    and the buffer holds a NUL-terminated string
      A = 0xFF and the buffer is untouched
```

The dispatcher answers this out of `emu_host_file_get_write_name()`, which you
already implement, and it is entirely in the shared core - **you get it by
rebuilding.** `getTrapTypeFromFunc()` widened its extension range from
0xE0-0xE7 to 0xE0-0xEF to route it; there was nothing else in 0xE8-0xEF.

`W8.COM` calls it right after opening the host file and prints the answer. A
failure is not an error: an emulator built before this existed returns 0xFF
from its unknown-function path, and W8 then prints the path that was asked for,
exactly as it always did. So a current `w8.com` runs unchanged on an
already-released front end - which matters, because your bundled disk images
and the released `hd1k_*` assets do not update when this repo changes.

One thing to mirror if you ever reimplement the dispatch side: `C` is a single
byte, so a destination longer than 254 characters does not fit - and it is
reachable, because a bare name gets the whole working directory prepended. The
handler does **not** clamp and return success; a path chopped at the front can
name a real directory or even a different real file, which is the failure this
call was added to remove. It keeps the *end* of the path, behind a leading
`...` so the answer reads as a fragment rather than as a path.

Tests: `tests/hbios_hostname.cc`, 29 checks, including the buffer bound (the
terminator must land inside it, byte C of the buffer must not be touched, the
truncation marker) and both ends of the widened range.

## 2. New: emu_host_path_basename() in the shared core

```cpp
std::string emu_host_path_basename(const std::string& path,
                                   const char* fallback = "download.bin");
```

In `emu_io_common.cc`, which every port already compiles. It reduces a path to
its last component for a backend that has no filesystem to honour a directory
with. It accepts **both separators**, because the string comes off a guest
command line that may have been typed on any host, and it never returns
something that would escape the directory it is joined to: `""`, `"."`, `".."`
and a bare drive letter all become the fallback. `"a/b/"` gives `"b"`.

This existed as three different answers before: the browser backend split on
`/` only, so a Windows-shaped path became one long download name; the iOS
backend did not split at all, so `emu_host_file_open_write()` stored
`/USERS/ME/OUT.TXT` as the export *filename*. **If your backend cannot honour a
directory, call this instead of whatever it does now** (`emu_io_wasm.cc` in
this repo is the worked example, one line).

Tests: the last group in `tests/cli_hostfile.cc`, 12 checks.

## 3. Tightened: what emu_host_file_get_write_name() must return

It was "the name you were handed". It is now **the effective destination** -
what the bytes will actually land in, after everything your platform does to a
requested path - and it is **user-visible text**, because W8 prints it. The
full contract is in the comment above the declaration in `emu_io.h`. In short:

- Valid between a successful `emu_host_file_open_write()` and the matching
  `emu_host_file_close_write()`. Outside that window return `""` or `nullptr`;
  callers tolerate both. **Do not serve the previous transfer's name** - W8
  would print the wrong file for the next export.
- A failed open must leave nothing to report.

Per port, what that means:

- **z80cpmw** returns `g_hostWriteFilename`, the raw requested name, and
  `nullptr` when not writing (which is fine). Two changes: run it through
  `resolveHostPath()` so a bare name reports as the data-folder path it really
  goes to, and through the `resolveRealPath()` you already have, so an
  **installed MSIX build reports the redirected `LocalCache` path instead of
  the one the OS silently moved**. That is the single biggest win here for your
  port - it is the "I can't find my exported file" checklist item in your
  `docs/FILE_TRANSFER.md` answered at runtime.
- **ioscpm** stores the requested string verbatim as `g_host_write_filename`
  and hands it to the Swift layer as the export name - see section 0, which is
  the destructive consequence of the same line. Reduce it with
  `emu_host_path_basename()` at `emu_host_file_open_write()`, and return the
  Exports-folder location the Swift layer will actually write, so the CP/M user
  is told where to look.
- **cpmdroid** - same shape as ioscpm; not checked out here, so this is a
  request rather than a diff.
- **the web frontend here** is done: it uses the shared helper, and
  `To host:` now prints the download name.

## 4. R8.COM and W8.COM changed. Your disk images did not.

Both were rebuilt. If you bundle or download `hd1k_*` images, they still carry
the old ones, and none of the above is visible to your users until they are
refreshed. `z80cpmw/todo.txt` already tracks this for the pre-98eb6a1 `w8.com`;
this is the same item, one release further on.

The rebuild recipe is now a script rather than prose:

```
disks/rebuild_disk_utils.sh     # assemble both, install into every image
disks/verify_disk_utils.sh      # check every image against the sources
```

`verify_disk_utils.sh` runs inside `make -C src test`. Mind the diskdef if you
do it by hand: `hd1k_combo.img` is `wbw_hd1k_0` because of its 1 MB MBR prefix,
a plain 8 MB image is `wbw_hd1k`, and the wrong one lists garbage rather than
failing.

### Both sources lost their `ORG`, and that is not cosmetic

`org 0100h` was wrong for this toolchain. M80 assembles each utility as one
relocatable code segment and L80 bases a `.COM` at 0100h **by itself**, so the
ORG was applied on top of that base and pushed the code to 0200h behind 256
zero bytes. It ran only because CP/M loads the whole file at 0100h and the Z80
slides through 256 NOPs into the code. `w8.com` was built that way and `r8.com`
was not, which is why `verify_disk_utils.sh` could only ever check one of them.

With the ORG gone both build bare and both are checked - and the check confirms
that the shipped `r8.com` was byte-for-byte what `src/r8.asm` builds, which had
been an open question.

## 5. What W8 now does with `^Z`

It copies the file whole and drops only the run of `1Ah` at the **very end** -
the padding CP/M writes into a file's last record, and exactly what `R8` puts
there on import. So:

- a text file imported with `R8` comes back byte for byte, as before;
- a binary containing `1Ah` bytes is no longer cut at the first one;
- a file whose real content ends in `1Ah` loses that tail. CP/M stores no
  length, only whole 128-byte records, so nothing can tell those apart. This is
  the documented boundary, and it is a far smaller loss than the old rule.

The byte count W8 and R8 print is 32-bit now. It was the low 16 bits, so a
100000-byte transfer reported `34464`.

## 6. Nothing else in the platform API moves

`emu_io.h` gains one declaration (`emu_host_path_basename`) and one rewritten
comment block (`emu_host_file_get_write_name`). No signature changes, no
removals. If you compile the core and implement `emu_host_file_*`, you build
unchanged - but see section 3 before you call it done: it builds, and it will
report the wrong place, which is the failure this notice is about.
