# Downstream Update Notice - 2026-08-23 (core v1.35 -> v1.36)

Addressed to every port that compiles this core: the Windows port (`z80cpmw`),
iOS/macOS (`ioscpm`), Android (`cpmdroid`), and the web/WASM frontend in this
repo. The canonical integration contract remains
[../DOWNSTREAM.md](../DOWNSTREAM.md); this file is the dated to-do list for
this sync.

v1.36 is a console-keys release. It comes out of a cross-port sweep started by
a z80cpmw user's report, "Ctrl R exits me from CPM" (fixed there in e35f336,
shipped in 1.0.20). `^R` turned out to be clean in this repo, but the same
shape of bug was not: the CLI's own escape key is `^E`, WordStar cursor-up, it
was delivered to the guest *as well as* acted on, and the raw mode was handing
`^S`/`^Q` and `^V`/`^O` to the line discipline before the guest ever saw them.
The rule this produced is now a contract in ../DOWNSTREAM.md, "Platform
Contract: Ctrl-A..Ctrl-Z Belong to the Guest", and it applies to every port
whatever its UI toolkit. There is **one declaration removed from `emu_io.h`**
(section 3) and **one new return value** from `emu_console_read_char()`
(section 4); nothing else in the platform API moves.

## 1. Required: audit your own Ctrl-letter shortcuts

Read the new contract section in ../DOWNSTREAM.md and check your accelerator
table, menu items, key handlers and any settings that bind keys. A Ctrl-letter
you claim must be configurable, must default to reaching the guest, must not be
advertised once it is unbound, and must never be the only path to an
unconfirmed reset.

Where each port stood at the sweep, so nobody re-does it:

- **z80cpmw** was the one with the bug, and it is fixed (e35f336). Nothing
  further to do.
- **ioscpm** folds Ctrl+letter to 0x01-0x1A and forwards it
  (TerminalView.swift:525-544, :603-610); every app shortcut uses Command. Do
  **not** port `keyboard.ctrlRToCpm` here - there is nothing to switch off. Its
  open item is a Reset toolbar button with no confirmation, which is the fourth
  bullet of the contract reached by a tap instead of a key.
- **cpmdroid** maps ctrl + KEYCODE_A..Z to 1..26 (TerminalView.kt:306-312) and
  has no options menu at all. The thing to watch is future work: adding an
  ActionBar/Toolbar with `alphabeticShortcut` items would start claiming
  Ctrl-letters and recreate the Windows bug exactly.
- **the web frontend here** forwards `term.onData` bytes verbatim, now focuses
  the terminal on load, and now cancels `Ctrl+Shift`+letter (which xterm.js
  leaves to the browser) so `Ctrl+Shift+R` no longer hard-reloads the session
  away.

## 2. Free fixes you inherit, and the ones you have to make yourself

Nothing in the shared core filtered a control byte before this release and
nothing does now - the losses were all in platform layers. If yours puts a
terminal into raw mode, check two masks:

- Clear `IXON` in `c_iflag`, or `^S` and `^Q` are XON/XOFF flow control and
  never arrive. `^S` is WordStar cursor-left and `^Q` prefixes the entire `^Qx`
  family, so the diamond is half dead without it. Leave `IXOFF` alone: it only
  throttles a fast sender and never consumes a typed `^S`.
- Clear `IEXTEN` in `c_lflag`, or BSD/XNU eats `^V` (VLNEXT) and `^O`
  (VDISCARD) - they are gated on IEXTEN alone there, outside the ICANON block,
  so a Linux test cannot show the loss. `^O` is the whole WordStar
  onscreen-format prefix.

cpmemu's `enable_raw_mode()` (its commit 1584295) is the reference for the full
set of clears; the `c_lflag` and `c_iflag` masks here are identical to it.
There is exactly one clear it makes that this repo cannot: `c_oflag` OPOST must
stay set here, because every output path emits a bare `\n` and relies on ONLCR
to turn it into CR LF. Both repos deliberately leave `c_cflag` alone - forcing
CS8/PARENB would reprogram a real serial console's line parameters, and
clearing ISTRIP is what actually preserves the 8th bit.

If you also clear `ICRNL`, audit your LF-to-CR rewrites in the same commit. A
tty then delivers Enter as CR natively and a 0x0A is a genuine `Ctrl+J`, so a
surviving rewrite makes the two keys indistinguishable - but a pipe still ends
its lines with LF and must keep the rewrite. In this repo the split is: keep it
on the non-tty read, drop it on the tty read, and gate the shared peek path on
`stdin_is_tty`.

## 3. Required: `emu_console_check_ctrl_c_exit()` is gone from `emu_io.h`

The declaration (previously `emu_io.h:104`) and both in-repo definitions are
removed. It has never had a caller in any port: `ioscpm` (emu_io_ios.mm:155),
`cpmdroid` (emu_io_android.cpp:251) and `z80cpmw` (emu_io_windows.cpp:139) each
define it and nothing calls it. Dead code that looks like a live `^C`
interception is a trap for the next person auditing exactly this question,
which is how it surfaced.

**What to do:** delete your definition when you take this release. Nothing
links against it, so leaving it in place also compiles - it is your call
whether to carry a dead function. **Keep your `emu_console_check_escape()`**:
that one is still declared (`emu_io.h`) and is still live for the CLI, so a
stub must remain in every platform layer. Its contract gained one line: an
`escape_char` of 0 means no key is reserved, and the platform must then consume
nothing.

If you want a `^C` escape hatch, do not resurrect this one - cpmemu's is the
model: switchable by config and CLI flag, and the five presses have to land
within two seconds of the first (its commit 1584295).

## 4. Note: `emu_console_read_char()` can return `EMU_CONSOLE_RETRY`

`emu_io.h` now defines `EMU_CONSOLE_RETRY` (-2), returned when the console read
consumed a keystroke that belongs to the emulator rather than the guest. The
CLI uses it for the escape key: the byte is latched for
`emu_console_check_escape()` and the guest gets nothing, instead of getting the
byte *and* the debugger firing.

This is source-compatible and needs no work in a port that reserves no key -
your backend simply never returns -2. What it does require is that you not
treat a negative return as a byte: `hbios_dispatch.cc` now clamps in CIOIN and
VDAKRD, but if you have your own copy of that dispatch, a raw `-2 & 0xFF` would
hand the guest 0xFE.

## 5. CLI-only: the escape character can now be turned off

`--escape=none` (and `"escape": "none"` in the settings file) reserves nothing,
and the startup banner now says the chosen key is reserved by the emulator
rather than merely naming it. This is CLI code and changes no interface, but it
is worth copying as a posture: the default escape is `^E`, which is WordStar
cursor-up, and a user who runs WordStar needs a documented way to get that key
back. `"^@"` is accepted as a spelling of `none` rather than as a binding,
because terminals send NUL for Ctrl+Space.

## 6. Note for ports that symlink this repo

`ioscpm` reaches the core through symlinks, and `emu_io.h` is one of them
(iOSCPM/Core/emu_io.h). This release changes that header, so the symlink
carries the change automatically - unlike v1.35 there is no new file to link.
Check `git ls-files -s iOSCPM/Core/ | grep ^120000` still shows symlinks and
not flattened copies before you conclude you have taken this.

## Migration checklist

- [ ] Rebuild against v1.36
- [ ] Audit every host shortcut for a Ctrl-letter; make any survivor
      configurable and default it to the guest
- [ ] Confirm no reset/reboot is reachable from an unconfirmed keystroke
- [ ] Check your raw mode clears IXON (^S/^Q) and IEXTEN (^V/^O), and leaves
      IXOFF alone
- [ ] If you clear ICRNL, fix your LF-to-CR rewrites in the same commit
- [ ] Delete your `emu_console_check_ctrl_c_exit()` definition; keep
      `emu_console_check_escape()` and handle `escape_char == 0`
- [ ] Make sure no code path treats a negative `emu_console_read_char()` return
      as a character
- [ ] If your UI names a keyboard shortcut in a menu or help screen, rebuild
      that text from what is actually bound
