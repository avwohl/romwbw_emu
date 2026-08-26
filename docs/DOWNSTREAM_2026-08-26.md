# Downstream Update Notice - 2026-08-26 (core after v1.36, unreleased)

Addressed to every port that compiles this core: the Windows port (`z80cpmw`),
iOS/macOS (`ioscpm`), Android (`cpmdroid`), and the web/WASM frontend in this
repo. The canonical integration contract remains
[../DOWNSTREAM.md](../DOWNSTREAM.md); this file is the dated to-do list for this
sync. The previous notice, [DOWNSTREAM_2026-08-25.md](DOWNSTREAM_2026-08-25.md),
still applies if you have not taken it - it carries the `W8` host-path work and
an urgent ioscpm item, and this one assumes it.

**One thing here breaks your build, on purpose. Everything else is behaviour
inside the core.** Total work for a port that does not want the new feature:
four lines.

## 1. New required function: `emu_host_file_get_read_name()`

```cpp
// Which file emu_host_file_read_byte() is actually reading, as a string fit to
// show the person who typed the R8 command.  "" or nullptr when no read is
// open.  Valid only between a successful emu_host_file_open_read() and the
// matching emu_host_file_close_read().
const char* emu_host_file_get_read_name();
```

`emu_io.h` declares it and the core does **not** define it, exactly as it does
not define `emu_host_path_caps()`. A port that syncs these sources without
adding it fails to *link*:

```
undefined reference to `emu_host_file_get_read_name()'
```

That link error is the signal to read this file. It is not a request to
implement anything: **`return "";` is a correct, complete answer.** The new
HBIOS call then reports "no answer", and `R8` prints the path it was given -
which is exactly what it printed before this change. Take that and you are done.

It is a required symbol rather than a core default for the same reason the
capability function is: a port that has not looked at its own read path should
have to say so, once, rather than inherit an answer.

### What answering properly buys you

`R8`'s `Reading:` line used to print the path the CCP shouted, which is not the
file that was opened. On the CLI backend the difference is routine: the CCP
uppercases the whole command line, so a shouted path opens only on the
case-insensitive retry, and a bare name is relative to a working directory the
guest cannot see. Run on Linux, against a real directory named `MixedCase`
holding `source.txt`:

```
    typed     R8 <dir>/MIXEDCASE/SOURCE.TXT
    was       Reading: <dir>/MIXEDCASE/SOURCE.TXT     (the shouted string; on a
                                                       case-sensitive volume it
                                                       names nothing)
    now       Reading: <dir>/MixedCase/source.txt     (what the open resolved to)
```

The CLI answers with `realpath()` of whichever spelling actually opened. If your
backend resolves, redirects or sandboxes a read path, answer with where it
really went and `R8` will say so.

**The browser deliberately answers `""`.** A read there is a file *picker*: the
guest's string is a hint the user is free to ignore, so echoing it is wrong
rather than merely unhelpful, and the picked file's real name never reaches the
core. `emu_io_wasm.cc` returns `""` and says why. A port whose read is also a
picker should do the same.

## 2. New HBIOS function: `HBF_HOST_GETRNAME` (0xEA)

The read mirror of `HBF_HOST_GETNAME` (0xE8), down to the calling convention.
Nothing to implement - it is in `hbios_dispatch.cc`, which you compile.

```
    B = 0xEA
    C = size of the buffer at DE, including room for the terminator
    DE = buffer address
    A = 0    the buffer holds a NUL-terminated string
    A = 0xFF the buffer is UNTOUCHED - no read open, or the backend has no answer
```

An emulator predating it answers `0xFF` from the unknown-function path, which is
why `R8` treats a failure as "print what was asked for": a refreshed `r8.com` has
to keep running on an already-released front end. `0xEA` is inside the extension
range `getTrapTypeFromFunc()` already routes (0xE0-0xEF), widened for `0xE8`.

Both getters now share one bounded copy, `HBIOSDispatch::storeHostName()`, so the
"keep the end, mark the cut with `...`" rule cannot drift between them.

## 3. Behaviour changes inside the core - nothing to do, but know about them

- **`HBF_HOST_OPEN_R` and `HBF_HOST_OPEN_W` now FAIL a path with no terminator**
  in the first 256 bytes, instead of opening whatever those 256 bytes spelled.
  Both return `A = 0xFF` without calling your backend at all. `R8` and `W8` both
  build the path in a 128-byte buffer and terminate inside it, so nothing shipped
  can reach this; a guest program of your own might. 255 characters plus a
  terminator is the longest path that can be delivered.

- **`emu_host_path_basename()` caps its result at 255 bytes**
  (`EMU_HOST_NAME_MAX` in `emu_io.h`), keeping the **extension** and the front of
  the stem. If you were relying on it to return the whole component, it no longer
  does for one longer than a filesystem would take anyway. A cut never lands
  inside a UTF-8 sequence.

- **The `.COM` files in `disks/*.img` changed.** `r8.com` and `w8.com` were
  rebuilt: `R8` prints the opened file and labels the path in its failed open,
  `W8` labels the path in its failed open and tells a CP/M read error from end of
  file. That last one matters to you if you ship images running ZSDOS or CP/M 3:
  `W8` used to read any nonzero `F_READ` status as EOF, so a real read error was
  reported as `Done: <n> bytes` for a truncated export. Under CP/M 2.2, where 1
  is the only nonzero code, the old behaviour was exact and this changes nothing.

  The release-order constraint from
  [RELEASE_ORDER_2026-08-25.md](RELEASE_ORDER_2026-08-25.md) still governs how a
  refreshed image reaches users.

- **`W8` and `R8` print a failed open in two lines now:**

  ```
  Error: Cannot create host file
    Asked for: /NODIR/X.COM
  ```

  If you scrape either utility's output, that is the shape that changed. The path
  is labelled because it is the *request*, not a destination - nothing was
  created, and it is in the case the CCP shouted rather than the case the
  emulator would have used.

## 4. Not for ports, but in the same sync

`emu_io_cleanup()` on the CLI no longer closes the printer/aux files; they close
through `atexit`. That path is not only process exit - the `sim>` debugger calls
cleanup and then init around every prompt - so anything closed there and not
reopened by init was closed for the run. Nothing reaches it today; the aux family
still has no caller. If your port's `emu_io_cleanup()` closes state that must
survive a mode switch, it has the same shape of bug.
