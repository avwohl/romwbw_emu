# romwbw-batch: CP/M commands with nobody at the keyboard

`tools/romwbw-batch` boots CP/M 2.2 on the emulator, runs a list of commands,
waits for them to finish, and hands back the files you ask for:

```bash
tools/romwbw-batch -t hello.asm -c "ASM HELLO" -c "LOAD HELLO" -g HELLO.COM
```

It is the harness under [ISX.md](ISX.md)'s Intel PL/M-80 recipes, and it is
general: anything you would type at `A>` in a CP/M 2.2 session, it can run.

## What it does

1. **Copies the system disk.** `hd1k_cpm22` from the selected RomWBW release,
   through `tools/romwbw-get path` - the cached original is read-only - into a
   scratch image, with the ROM from the same release (`@rom`). `--disk` and
   `--rom` name other catalog ids or files.
2. **Adds your files**, to A: or to B: (see [Drives](#drives)).
3. **Writes the commands as A:$$$.SUB**, in the layout DRI's SUBMIT.COM writes:
   one 128-byte record per command, last command first, each a length byte,
   the text, 00H and `$`. The CCP runs them at boot as though SUBMIT had just
   been typed.
4. **Boots with stdin on /dev/null** (`--boot=2 --no-config`, nothing persisted),
   so the run ends by itself: after the last command the CCP reads the console,
   finds end of input, and the emulator winds down.
5. **Checks the batch reached its end** and extracts the files named with `-g`.

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
| `-g, --get [B:]PATTERN` | extract matching files after the run; `*` and `?` work |
| `-o, --out DIR` | where they go (default `.`), under their CP/M names |
| `--isx[=compact\|cbios]` | set the machine up for DRI's ISX - [ISX.md](ISX.md) |
| `--exact` | record exact file lengths the way ISX does, and trim by them on the way out |
| `--rom ID\|FILE`, `--disk ID\|FILE` | the ROM, and the CP/M 2.2 system disk to copy |
| `--emu PATH` | the emulator (default `$ROMWBW_EMU`, `src/romwbw_emu`, then PATH) |
| `--offline` | pass `--offline` to `romwbw-get` |
| `--timeout SECS` | kill the run after this long (default 900) |
| `--work DIR` | keep `a.img`, `b.img` and `console.log` here |
| `--log FILE` | write the console transcript here as well |
| `-v` | show the console as it runs |
| `-q` | no summary on success |

A command may be 125 characters; the CCP's buffer is 127 and SUBMIT.COM
refuses longer. Commands are passed as written; the CCP upper-cases them.

Exit status: **0** the batch completed and every `-g` matched something;
**1** it did not complete, or a `-g` matched nothing; **2** something it needs
is missing - the emulator, the ROM, the disk, `cpm_disk.py`; **64** usage;
**130** interrupted. The scratch directory, with its disk images, is removed
however the tool exits, unless `--work` named it.

It needs `cpm_disk.py`, cpmemu's disk tool, found as the rest of this
repository finds it: `$CPM_DISK`, then a sister `../cpmemu` checkout, then a
`cpm_disk` on PATH.

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
- **Ten billion instructions a run** - the emulator's own limit. The tool
  reports it when a run stops there.
- `-g` reads the directory after the run; a file a program deleted is gone.
