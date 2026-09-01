# Decisions

Questions that need a ruling from the owner. Nothing here is blocked on a
machine, a build or a measurement: each one has been measured already, and the
facts are recorded below so that answering it takes reading rather than
re-derivation. What is missing in every case is a choice.

**Why these are not in `todo.txt`.** That file's promise is that every item
carries a tag saying what a machine has to be able to do to take it, so a
session on another OS can see at a glance what it can pick up. `[DECISION]` was
never such a tag - it meant the opposite, that no machine can take the item at
all. And the file already established what to do with work that needs a person:
that is what [`MANUAL_CHECKS.md`](MANUAL_CHECKS.md) is. A check needs someone at
a keyboard; a decision needs someone making a ruling. Both were the same kind of
exception and only one of them had been split out. The cost of leaving them in
was measured: the 2026-09-01 audit re-verified all six, confirmed the facts
still held, and moved none - which is what every future audit will do, because
the blocker is judgment and no amount of evidence supplies it.

**Delete a decision once it has been answered.** If the answer needs work done,
that work becomes a `todo.txt` item with a real capability tag. If the answer is
that nothing changes, it goes in `CHANGELOG.md` so the question stays answered.
Either way the entry leaves this file. An answered decision left in place turns
this file into the same accumulating record `todo.txt` was.

The sections are independent and in a fixed order; answering one does not
require reading any other.

---

## 1. `--escape=none`, or a doubled escape character

**The question.** `--escape=none` currently gives the guest every key and gives
up the `sim>` prompt altogether. Should it stay that way, or should the
all-or-nothing choice be replaced by a doubled escape character - one press
reaches the guest, two in a row reach `sim>`?

**What is true today.** With `--escape=none`, `console_escape_char` is 0, and
`check_console_escape_async()`, `poll_stdin()` and `emu_console_check_escape()`
all take their early-out. There is no way to reach `sim>` in that mode.

**What each answer costs.**

- **Leave it as it is.** Nothing to build. `--escape=none` remains a trade the
  user makes knowingly: every key to the guest, no simulator prompt.
- **Build the doubled escape character.** It costs a pending-escape state and a
  rule for delivering a lone escape late, and it lands in
  `emu_console_check_escape()` - a function `DOWNSTREAM.md` makes a contract
  that three ports keep, so the behaviour change is theirs too.
- **Delete this entry.** The original item ends "nobody has asked for it.
  decide it or delete this item", and that stands as an option in its own
  right.

**Who is blocked.** Nobody is waiting - no port, release step or other item is
held up by the answer. But a yes is not local: the change lands in
`emu_console_check_escape()`, which `DOWNSTREAM.md` makes a contract three ports
keep, so the new behaviour would be theirs to implement as well. The one fact
worth weighing against building it is the recorded one: nobody has asked for the
doubled escape.

---

## 2. A real second character device, or delete the printer/aux family

**The question.** `LST:`, `PUN:` and `RDR:` are aliased to the console. Should
the emulator grow a real second character device - which moves `LST:` and `PUN:`
off the console for everyone using it today - or should the seven unreachable
printer/aux functions be deleted?

**What is true today.** The whole aux/printer family - `emu_printer_set_file`,
`emu_printer_out`, `emu_printer_ready`, `emu_aux_set_input_file`,
`emu_aux_set_output_file`, `emu_aux_in`, `emu_aux_out` - has no caller anywhere
and no flag to reach it. Measured under CP/M 2.2 on `hd1k_combo.img`:
`STAT DEV:` reports `LST:` is `LPT:` and `PUN:` is `PTP:`, and
`PIP LST:=SOURCE.TXT` prints on the terminal. The reason is that `SYSGET_CIOCNT`
answers 1, so CBIOS has one character device to assign all four names to.

**What each answer costs.**

- **A real second device.** Wiring `emu_printer_out()` up is not "add a flag".
  It means answering 2 to
  `SYSGET_CIOCNT` and routing CIO unit 1, which MOVES `LST:` and `PUN:` off the
  console for everyone using it today. Every existing setup that prints to the
  terminal by way of `PIP LST:=` changes behaviour.
- **Delete the seven functions.** Costs nothing at runtime - they have no
  caller - and removes the appearance of a feature that does not exist.

**Who is blocked.** The second `[BROWSER]` item in `todo.txt` overlaps this one:
the wasm build emits `Module.onPrinterOutput`, which the page never implements,
and that item's own choice is "wire it properly behind a browser, or delete one
side". A deletion here decides the printer half of that item; a real second
device gives the browser callback something to carry, and then the two have to
be settled together rather than separately.

---

## 3. What `emu_console_write_char()` does with CR, and with bit 7

**The question.** Two separable questions about the same function. (a) Should
console output stop dropping CR, so that a progress line can redraw? (b) Should
it stop masking every output byte to 0x7F?

**What is true today.** `emu_console_write_char()` drops CR and masks every
output byte to 0x7F, in `emu_io_cli.cc` and identically in `emu_io_wasm.cc`. So
a progress line never redraws - `"100%\rDONE"` arrives as `"100%DONE"` - and
`0xC3 0xA9` arrives as `C )`, a different printable character rather than a
mangled one. The 0x7F mask has a recorded reason: WordStar sets bit 7 on the
last character of a word and expects a dumb terminal to strip it.

**What each answer costs.**

- **Stop dropping CR.** The change is cross-cutting, not local. A piped run
  would gain CR LF where it yields bare LF today, so every captured transcript
  and every script that greps the output changes. The page's
  `Module.onConsoleOutput` rewrites LF to CR LF specifically to undo the drop,
  so the two have to move in one commit or xterm gets CR CR LF. And
  `romwbw-debug.html` has its own handler, which is a third site.
- **Keep dropping CR.** No progress line, no `\r` redraw, ever - which is what
  the emulator does today.
- **Stop masking to 0x7F.** Against the WordStar reason stands every guest that
  draws a box or writes a national character; those are the cases the mask
  breaks.
- **Keep the 0x7F mask.** WordStar's last-character-of-a-word convention keeps
  working without a terminal that strips bit 7 itself.

**Who is blocked.** Every port, though none is waiting on an answer.
`emu_console_write_char()` is the first entry in `DOWNSTREAM.md`'s "Key
functions to implement" list, so what the core hands it - a CR or no CR, seven
bits or eight - is what every front end receives, and a change here is a change
each of them sees. Inside this repository the CR half also cannot be done
piecemeal: `emu_io_cli.cc`, `emu_io_wasm.cc`, the page's
`Module.onConsoleOutput` and `romwbw-debug.html` are one commit's worth of work
or the web build is visibly wrong in between.

---

## 4. What a package ships: the duplicated ROM, and the missing disk

**The question.** Two halves, and one answer covers both. (a) The four ROM files
in this tree are one file - which name is canonical, and what does the web build
fetch? (b) Should the deb and rpm carry a disk image at all?

**What is true today.** `web/emu_avw.rom`, `web/emu_romwbw.rom`,
`roms/emu_avw.rom` and `roms/emu_romwbw.rom` all md5 to
`826c3bcf7db18a36f8eb73792873613d`, so deduplicating them means deciding which
name is canonical and what the web build fetches - the page's ROM select offers
`emu_avw.rom` by name. Separately, no `.img` is staged by `release.yml` or by
`web/makefile`'s deploy targets, so a stock deb or rpm has no disk at all, while
the page offers five disk names of which `z80cpm_tools.img` exists nowhere in
this tree.

**What each answer costs.**

- **Deduplicate the ROMs.** Whichever name loses, the page's ROM select and the
  web build's fetch have to name the survivor. The four copies are byte
  identical, so nothing about the emulator's behaviour changes.
- **Leave the four copies.** Three redundant copies of one ROM stay in the tree.
  A package carries fewer than four but still more than one: `release.yml`
  stages `roms/*.rom`, and then stages `roms/emu_avw.rom` a second time next to
  the page, because the page fetches its ROM by a bare relative name.
- **Ship an image in the packages.** A 49 MB combo image in a deb is a
  packaging decision in its own right - size, build time and mirror bandwidth
  for every install.
- **Ship no image.** The status quo: a stock install 404s on every `.img` name
  the page's select offers, including the two the page selects by default, and
  the user loads an image through the page's file picker instead. The CLI is
  unaffected, since it takes a path.

**Who is blocked.** `README.md`'s "**No disk image is packaged.**" paragraph
points here for this question - it was repointed from `todo.txt` when this file
was created. The `[1.37]` entry in `CHANGELOG.md` still says "still open in
`todo.txt` as a policy question", and is deliberately left alone: it was true
when it shipped, and this repository does not rewrite released entries.
`todo.txt`'s header is what catches a reader who follows it. `release.yml` and
`web/makefile`'s deploy targets are what would change if the answer is to ship
an image.

---

## 5. Whether `emu_host_file_open_write()` is allowed to succeed and then fail

**The question.** Should `W8` be able to tell "the file was never opened" from
"the file was opened and the close failed"? Saying yes means changing the
`HBF_HOST_*` return codes that four ports implement.

**What is true today.** `emu_host_file_open_write()` returns true
unconditionally on every buffering backend - browser, iOS, Android, and
`z80cpmw`'s deferred write - so a path that cannot be created is only discovered
at close, and `W8` then prints `"Host file close failed - file may be
truncated"` for a file that was never opened. `emu_io.h`'s contract now says
that is allowed, and that close is where the real answer comes from.

**What each answer costs.**

- **Leave it.** `emu_io.h`'s contract already documents the behaviour, so this
  is consistent rather than accidental. `W8`'s message stays misleading in the
  never-opened case.
- **Make `W8`'s two messages tell the cases apart.** It needs a distinguishable
  status from close, which changes the `HBF_HOST_*` return codes four ports
  implement: a cross-port ABI decision, not a local edit.

**Who is blocked.** The four ports that implement the `HBF_HOST_*` return codes
are the ones a yes commits. Keep them distinct from the buffering backends named
above - browser, iOS, Android and `z80cpmw`'s deferred write - which are what
returns true unconditionally today; the source kept the two groups separate and
this file should not merge them. Nothing waits on a no.

---

## 6. Whether a config-file `boot` key should be written back to NVRAM

**The question.** A `boot` key read from the config file is written back to the
NVRAM file at exit; a `--boot` on the command line applies to that run only.
Is that asymmetry right, or should the config-file value be treated the same way
as the command-line one? A second, smaller question sits in the same place:
`--boot=none` names the pre-XDG setting in `~/.config` rather than removing it,
and that restraint is either right or a leftover.

**What is true today.** A config-file `boot` key is still written back to the
nvram file at exit, while a `--boot` on the command line applies to that run
only. `--boot=none` likewise names a pre-XDG setting in `~/.config` and leaves
it in place, rather than reaching outside the config directory the run selected.

**What each answer costs.**

- **Leave it.** The asymmetry is deliberate: `romwbw_emu.json` IS a persisted
  choice, so writing it through is at worst redundant.
- **Change it.** Only worth doing if something starts generating
  `romwbw_emu.json` per run - at which point a per-run file would be silently
  persisting itself into NVRAM, and this is the entry to re-read.

**Who is blocked.** Nobody. This is recorded as a deliberate choice with a
trigger for revisiting it, not as an open defect.
