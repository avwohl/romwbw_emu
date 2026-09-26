# romwbw-batch: CP/M commands with nobody at the keyboard

`tools/romwbw-batch` boots CP/M 2.2 on the emulator, runs a list of commands,
waits for them to finish, and hands back the files you ask for:

```bash
tools/romwbw-batch -t hello.asm -c "ASM HELLO" -c "LOAD HELLO" -g HELLO.COM
```

It is the harness under [ISX.md](ISX.md)'s Intel PL/M-80 recipes, and it is
general: anything you would type at `A>` in a CP/M 2.2 session, it can run.

Installed from the .deb or the .rpm, it is `romwbw-batch` on PATH, beside
`romwbw_emu`, `romwbw-get` and `romwbw-plm80`, and this file, [ISX.md](ISX.md)
and `isxbios.asm` are in `/usr/share/doc/romwbw-emu/`. The examples here are
written for a checkout, as `tools/romwbw-batch`.

## What it does

1. **Copies the system disk.** `hd1k_cpm22` from the selected RomWBW release,
   through `tools/romwbw-get path` - the cached original is read-only - into a
   scratch image, with the ROM from the same release (`@rom`). `--disk` and
   `--rom` name other catalog ids or files.
2. **Adds your files**, to A: or to B: (see [Drives](#drives)), each
   replacing any file of the same name there - see
   [Your files and the system disk's](#your-files-and-the-system-disks).
3. **Writes the commands as A:$$$.SUB**, in the layout DRI's SUBMIT.COM writes:
   one 128-byte record per command, last command first, each a length byte,
   the text, 00H and `$`. The CCP runs them at boot as though SUBMIT had just
   been typed.
4. **Boots with stdin on /dev/null** (`--boot=2 --no-config`, nothing persisted),
   so the run ends by itself: after the last command the CCP reads the console,
   finds end of input, and the emulator winds down.
5. **Checks the batch reached its end** and extracts the files named with `-g`,
   warning of any from A: that is still the system disk's own - see
   [Your files and the system disk's](#your-files-and-the-system-disks).

**How it knows the batch finished.** "The emulator exited" does not say that -
the emulator also stops when a command fails in a way that waits for a key.
So the tool adds `ROMWBW.END`, holding a string it makes up for the run, and
ends the batch with `TYPE ROMWBW.END`. The run counts as complete only if that
string reaches the console. A command the CCP cannot find (`NOSUCH?`) deletes
`$$$.SUB`, as does an ISX error, so the string never comes and the tool prints
where the console stopped.

**Why not piped stdin.** Programs read ahead: the BDOS keeps any key it sees
while printing, DDT stops a listing on one, a BDOS error takes the next key as
"any key". A submit file races nothing. [ISX.md](ISX.md#piped-stdin) has the
detail, including the boot loader's appetite for typed-ahead input, which the
emulator now holds back.

## Options

| | |
|---|---|
| `-a, --add [B:]PATH[=NAME]` | add a host file, binary; a directory adds every file in it |
| `-t, --add-text [B:]PATH[=NAME]` | add a text file: LF becomes CR LF, it ends at its first `^Z`, and it is padded with `^Z` |
| `-c, --cmd LINE` | a command, run in order; repeat it |
| `-s, --script FILE` | commands from a file, one a line; `;` and `#` lines are skipped |
| `-g, --get [B:]PATTERN` | extract matching files after the run; `*` and `?` work; a file from A: that is still the system disk's own draws a warning |
| `-o, --out DIR` | where they go (default `.`), under their CP/M names |
| `--isx[=compact\|cbios]` | set the machine up for DRI's ISX - [ISX.md](ISX.md) |
| `--exact` | record exact file lengths the way ISX does, and trim by them on the way out |
| `--rom ID\|FILE`, `--disk ID\|FILE` | the ROM, and the CP/M 2.2 system disk to copy |
| `--emu PATH` | the emulator (default `$ROMWBW_EMU`, the `romwbw_emu` beside the tool, `src/romwbw_emu`, then PATH) |
| `--offline` | pass `--offline` to `romwbw-get` |
| `--timeout SECS` | kill the run after this long (default 900) |
| `--max-instructions N` | have the emulator stop the run after N Z80 instructions (default ten billion, the emulator's own for a run whose stdin is not a terminal); 0 for no limit |
| `--work DIR` | keep `a.img`, `b.img` and `console.log` here |
| `--log FILE` | write the console transcript here as well |
| `-v` | show the console as it runs |
| `-q` | no summary on success |

A command may be 125 characters; the CCP's buffer is 127 and SUBMIT.COM
refuses longer. Commands are passed as written; the CCP upper-cases them.

Exit status: **0** the batch completed and every `-g` matched something; **1**
it did not complete, or a `-g` matched nothing; **2** something it needs is
missing - the emulator, the ROM, the disk, `cpm_disk.py`; **64** usage, a file
to add or a `--script` that is not there among it, all checked before the ROM
and the disk are looked for; **130** interrupted. The scratch directory, with
its disk images, is removed however the tool exits, unless `--work` named it.

It needs `cpm_disk.py`, cpmemu's disk tool, and looks for it in this order:
`$CPM_DISK`; a cpmemu checkout beside this one, as the rest of this
repository finds it; the copy the .deb and .rpm install for it,
`/usr/share/romwbw_emu/cpm_disk.py` (found beside the tool's own directory,
as `../share/romwbw_emu/`); then a `cpm_disk` on PATH, which cpmemu's own
package and `make install` put there. The packaged copy comes before PATH
because it is the one the release was built with: a cpm_disk.py without
`delete_file(..., exact=)`, which came with cpmemu 4.10.0, is refused by name
rather than failing half way through a batch. Found nowhere, the error says
where it looked.

## Drives

A: is slice 0 of the system disk. On a RomWBW disk boot B: is the RAM disk,
which starts empty and is lost with the run, so files added to B: go instead
onto a second hd1k image attached as `--disk1`, and the batch starts with

```
ASSIGN B:=HDSK1:0,G:=MD0:0
```

which swaps it in. (ASSIGN refuses a map in which two letters name one
filesystem - `ASSIGN B:=HDSK1:0` alone says "Multiple drive letters reference
one filesystem, aborting!" - hence the swap.) That is what lets DRI's ISX
recipes, which keep their tools on `:F1:`, run as they were written. With
`--isx=compact` the swap is issued again after every `CPM`, because leaving ISX
that way reboots CP/M, and B: is attached even when nothing is added to it:
the compact BIOS has only the drives the batch attaches, and ISIS programs put
their work files on `:F1:` - PL/M-80 its five - so without it the first one
ended the batch with `Bdos Err On B: Select`. (Under `--isx=cbios` an empty B:
is the RAM disk, which serves.)

## Your files and the system disk's

A: is not empty. It starts as a copy of the system disk, and `hd1k_cpm22`
holds 95 files of RomWBW's own in 3.6.0 - ASM.COM, ED.COM, PIP.COM, STAT.COM,
MBASIC.COM, a README.TXT, and a HELLO.ASM among them. A file you add under a
name that is already there **replaces** RomWBW's in the scratch copy:
`romwbw-batch -t HELLO.ASM ...` assembles your HELLO.ASM, not RomWBW's sample,
and `-a ED.COM` of your own is the ED every later command runs. Nothing warns
you, since it is usually what you meant; the cached original is never touched,
so the next batch starts from RomWBW's files again. B:, a fresh image, has
nothing to replace.

The other direction works the same way: `-g` extracts from A: as the run left
it, so `-g *.COM` brings back ASM.COM, PIP.COM and the rest of RomWBW's along
with whatever the batch made - and a build that was to replace one of them and
failed leaves RomWBW's there, so `-g STAT.COM` after a failed rebuild of STAT
brings back the stock STAT.COM. The tool says so: a file `-g` extracts from A:
that is byte for byte the system disk's own draws a warning on stderr,

```
romwbw-batch: STAT.COM is the system disk's own, byte for byte - the batch did not change it
```

and several are named in one line (`71 of the files extracted are the system
disk's own ...` for `-g *.COM` after a `DIR`). It is a warning, not a failure:
the exit status is as it would be, since RomWBW's PIP.COM may be just what you
asked for. B: starts empty and never draws it. From Python,
`Batch.is_system_original(name)` asks the same question. (`romwbw-plm80` goes
further for its own outputs: it erases their names from A: before the run, so
a file of that name afterwards is the build's, and a build that made none
fails - see [ISX.md](ISX.md).)

## From Python

The script loads as a module, and the class under the command line is
`Batch`:

```python
import importlib.machinery, importlib.util
loader = importlib.machinery.SourceFileLoader("rb", "tools/romwbw-batch")
spec = importlib.util.spec_from_loader("rb", loader)
rb = importlib.util.module_from_spec(spec); loader.exec_module(rb)

b = rb.Batch(isx="compact", offline=True)
b.add_dir("PLM_WORK", drive="B")
b.add("T1.PLM", text=True)
r = b.run(["B:ISX", ":F1:PLM80 T1.PLM", ":F1:CPM"])
assert r.completed, r.tail()
obj = b.read("T1.OBJ")
b.cleanup()
```

`tools/romwbw-plm80` is built this way.

## Limits

- **A CP/M 2.2 CCP.** The submit file is read by the CCP at boot; CP/M 3 reads
  `$$$.SUB` differently and is not supported. `hd1k_cpm22` is the default.
- **A: and B: only**, and user area 0.
- **Ten billion instructions a run**, by default - the emulator's own limit
  for a run whose stdin is not a terminal, which a batch's never is (it is
  `/dev/null`); some seven minutes at full speed. `--max-instructions` moves
  it and 0 removes it. A run stopped there exits 124 from the emulator, and
  the tool says the batch did not complete and why.
- `-g` reads the directory after the run; a file a program deleted is gone.
