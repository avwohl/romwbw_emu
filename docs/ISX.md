# Intel's ISIS-II tools under DRI's ISX, on RomWBW

Digital Research did not build MP/M II's utilities with a DRI compiler. They
wrote them in PL/M and compiled them with **Intel's** PL/M-80 V3.1, linked and
located them with Intel's LINK and LOCATE, all ISIS-II programs, and ran those
on CP/M through **ISX** - "ISIS-II INTERFACE VERS 1.4", a DRI program that
makes a CP/M 2.2 machine look like an Intel MDS to an ISIS program. The MP/M II
source release carries the whole kit in `mpm2src/PLM_WORK`, with the submit
files DRI built with.

`tools/romwbw-batch --isx` runs that kit on this emulator, unattended, and
`tools/romwbw-plm80` wraps DRI's two recipes. Rebuilt this way, 23 of DRI's 24
PL/M binaries in the MP/M II source tree come out **byte for byte identical**,
and the 24th differs only in bytes the program never initialises (see
[Results](#results)). [BATCH.md](BATCH.md) documents `romwbw-batch` itself.

None of DRI's or Intel's binaries is in this repository. Point the tools at
your own copy of `PLM_WORK`.

## What ISX is

ISX.COM is 12,544 bytes and loads at 0100H like any CP/M program. It holds:

- **a CCP of its own**, prompting `0>` (the current drive), with a few built-in
  commands - `DIR`, `ERA`, `TYPE`, `REN`, and `DBUG` - and otherwise loading
  the ISIS program the command names, from the file of that name with no
  extension (`PLM80`, `LINK`, `LOCATE`, `OBJHEX`, ...). It cannot run a CP/M
  `.COM`;
- **a BDOS of its own**, a CP/M 2.2 clone (it prints `Bdos Err On`), which the
  ISIS system calls are translated to, and which calls the BIOS directly;
- **the ISIS-II system calls** at 0040H, as ISIS programs expect them;
- **the MDS monitor's entry vector**, copied at start-up to F800H-F87FH, since
  Intel's tools call the monitor for console I/O and to ask how much memory
  there is (MEMCK, at F81BH).

ISIS keeps 0000H-367FH for itself and loads programs at 3680H, so ISX fits in
the part of memory ISIS programs never touch.

Returning to CP/M is the ISIS program `CPM` in `PLM_WORK`: 13 bytes at 3680H
that jump through 0001H to the BIOS warm boot.

### How ISX takes its commands

- **From the console**, a line at a time, through its BDOS's function 10.
- **From A:$$$.SUB**, exactly as the CP/M 2.2 CCP does: when the file is there
  ISX reads its last record, shortens the file by one record, and echoes the
  line - with BDOS function 9, so up to a `$`. DRI's SUBMIT.COM writes each
  record as a length byte, the text, 00H and `$` (dumped from a `$$$.SUB` it
  wrote), and that `$` is what stops the echo; a record without one has ISX
  print memory until it happens on one. This is how DRI's submit files mix
  CP/M and ISIS commands: `isx` on one line, ISIS commands on the next, `cpm`,
  then CP/M commands again - one `$$$.SUB`, read in turn by two command
  processors.
- **Not from its command tail.** `ISX DIR B:` does not run `DIR B:`; the tail
  confuses ISX's start-up, which on RomWBW answered `Bdos Err On G: Select`.
- An unknown command (`:F1:SETEOF?`) aborts the submit file, as the CCP's `?`
  does.

`:F0:` is drive A:, `:F1:` is B:, and so on: `:Fn:` is CP/M drive *n*.
`DIR :F1:` and `DIR B:` list the same thing.

### What it needs from the machine

Read from its start-up code (3000H and 199AH in ISX.COM):

| | |
|---|---|
| **The BIOS base** | taken as `(0001H) - 3`, and assumed to be on a page boundary: ISX calls BIOS entries as `LHLD 0001H / MVI L,nn / PCHL`. If the base is F700H or above, ISX first copies the 17-entry jump table down to F700H and moves 0001H with it. |
| **All memory up to the BIOS** | ISX points 0006H at the BIOS base and patches the BIOS's cold-boot jump to its own BDOS at 0103H, so `CALL 5` reaches ISX. MEMCK answers `(0006H) - 1`: ISIS programs may use everything from 3680H to the BIOS, **the CP/M BDOS and CCP included**. |
| **F800H-F87FH** | the monitor vector, written over whatever is there. |
| **0040H** | the ISIS entry. RomWBW keeps a page-zero stamp there; its warm boot puts it back. |
| **IOBYTE** | the monitor's IOCHK and IOSET read and write 0003H. |
| **A warm boot that reloads the BDOS** | because the ISIS program before `CPM` may have overwritten it. |
| **Exact file lengths** | below. |

**Exact file lengths.** ISIS files are byte streams; CP/M files are 128-byte
records. ISX keeps the number of *unused* bytes in a file's last record in
directory byte 13 (S1) - measured on files it wrote: T1.OBJ's EOF record ends
88 bytes before the end of its last record, and its S1 is 88 (58H). DRI's
submit files run `SETEOF` on each source first; SETEOF counts the `^Z` padding
into FCB byte 13 and closes the file. But a stock CP/M 2.2 BDOS does not copy
S1 back to the directory on close - only the allocation map, extent and record
count - so on this machine SETEOF changes nothing, and PL/M-80 reads the
padding: 77 `UNPRINTABLE ASCII CHARACTER` errors for a 435-byte source.
`romwbw-batch --isx` writes S1 itself when it adds a file, and trims by it when
it extracts one.

## Why it does not simply run on RomWBW

RomWBW's CP/M 2.2 puts the CCP at D000H, the BDOS at D800H and a 6.5 KB CBIOS
at E600H-FDFFH. Against the list above:

1. **The warm boot does not reload the BDOS.** `cbios.asm`, WBOOT: "WE DON'T
   WANT TO RELOAD BDOS" - only the CCP comes back, from a copy in the HBIOS
   bank. After PL/M-80 has used memory up to E5FFH, `CPM` lands the CCP on top
   of a BDOS full of symbol table. What that looked like: a run that printed
   memory, a run that hung, a run that exited at once. ISX and `CPM` with no
   ISIS program between them works, which is what narrowed it down.
2. **E5FFH is not enough memory for PL/M-80.** Compiling the MP/M II sources
   with the reference ISIS emulator at different MEMCK values: SET, SHOW and
   STAT fail with `LIMIT EXCEEDED: DYNAMIC STORAGE` at E5FFH and compile at
   EBFFH; ED crashes at E5FFH and compiles at EBFFH; PIP fails at EFFFH and
   compiles at F1FFH.
3. F800H-F87FH is inside the CBIOS. It turns out to be free: it is the top of
   the CBIOS buffer heap, and with the drive map this machine builds, 1859
   bytes of that heap are unused - from F67DH to FDBFH.

`romwbw-batch` has two answers, `--isx=cbios` and `--isx=compact`.

### `--isx=cbios`: ISX on RomWBW's own BIOS

ISIS memory stops at E5FFH, so this is enough for most programs and not for
the largest. `romwbw-batch` replaces `CPM` with an ISIS module that copies the
BDOS back from the image on the system tracks - the boot record on sector 2
says where it is, and the image starts on sector 3 - puts back the BIOS
cold-boot jump ISX patched, and then does what DRI's `CPM` does. That is the
reload a CP/M 2.2 BIOS traditionally did on warm boot. The tool also checks,
after the run, that the CBIOS heap left F800H-F87FH free.

### `--isx=compact` (the default): ISX on a BIOS of its own

[tools/isxbios.asm](../tools/isxbios.asm) is a 403-byte CP/M 2.2 BIOS for ISX
alone: the console through HBIOS CIO, and A: and B: as HBIOS disk units 2 and 3
(the batch's `--disk0` and `--disk1`), with a one-sector write-through
deblocking buffer. It sits at F600H, so ISIS memory runs to F5FFH - 4 KB more
than E5FFH, and enough for every MP/M II utility.

`romwbw-batch` installs it by rewriting ISX.COM's first instruction, JMP 3000H,
to jump instead to 39 bytes it appends at 3200H. Those ask HBIOS for the user
bank, copy the BIOS to F600H, point 0001H at its warm boot, put the JMP 3000H
back and jump to it. ISX's own start-up then finds a BIOS below F700H, leaves
it there, and does the rest itself:

```
0100-31FF  ISX                         F600-F7FF  isxbios: jump table, code, tables
3680-F5FF  ISIS programs               F800-F87F  ISX's monitor vector
                                       F880-FCFF  isxbios: buffers
                                       FE00-FFFF  HBIOS proxy, untouched
```

The CBIOS does not survive the ISIS programs, so the compact BIOS's warm boot
asks HBIOS for a reset: the boot loader runs, boots CP/M from the same disk,
and the CCP carries on with `$$$.SUB` - so DRI's `CPM`, which jumps through
0001H, needs no replacement. The reboot rebuilds the CBIOS drive map, so
`romwbw-batch` re-issues its `ASSIGN` for B: after every `CPM`. Everything the
compact BIOS writes goes to the disk at once, so nothing is lost by the reset.

## The recipes

`tools/romwbw-plm80` runs DRI's two routes. Everything given with `--tools`
goes to B:, which ISX calls `:F1:`; the sources and `--include` files go to A:
(`:F0:`), where PL/M-80 looks for `$INCLUDE` files.

**`com`** is P.SUB and C.SUB, which make a CP/M .COM at 0100H:

```
b:is14                                  ->  B:ISX
:f1:PLM80 $1.PLM debug PAGEWIDTH(80)    ->  :F1:PLM80 NAME.PLM DEBUG PAGEWIDTH(80)
:f1:link $1.obj,:f1:x0100,:f1:plm80.lib ->  :F1:LINK NAME.OBJ,:F1:X0100,:F1:PLM80.LIB TO NAME.MOD
    to $1.mod
:f1:locate $1.mod code(0100H)           ->  :F1:LOCATE NAME.MOD CODE(0100H) STACKSIZE(100) MAP PRINT(NAME.TRA)
    stacksize(100) map print($1.tra)
:f1:cpm                                 ->  :F1:CPM
e:objcpm $1                             ->  B:OBJCPM NAME
```

`X0100` is an object module of nothing but EQUs - MON1, MON2 and MON3 at 0005H,
FCB at 005CH, BOOT at 0000H - so a program declares them EXTERNAL. And since
Intel's PL/M-80 puts the procedures before the main program, a .COM needs DRI's
trick to start at 0100H: the first DATA in the module is a jump to the first
statement, less the three bytes of `LXI SP` in front of it:

```
declare start label;
declare jmp$to$start structure (jmp$instr byte, jmp$location address)
        data (0C3H, .start-3);
...
start:
    call main;
```

**`prl`** is PRL.SUB and DRI's PRLA*.SUB and PRLB*.SUB, which made the MP/M II
transients: link and locate twice, at 0100H with `X0100` and at 0200H with
`X0200`, convert both to hex with OBJHEX, return to CP/M, and

```
PIP NAME.HEX=NAME1.HEX,NAME2.HEX
B:ZERO                                  (clears memory, so unset bytes are 0)
B:GENMOD NAME.HEX NAME.PRL [$1000]
```

GENMOD, from MP/M II, makes the page-relocatable file from the difference
between the two images. `--data 1000` passes ED's and PIP's `$1000`,
`--no-zero` leaves out ZERO as PIP.SUB and SDIR.SUB did, `--stack`,
`--plm-options` and `--x0100 first|last|none` cover the rest of what the
individual submit files vary. Several sources are compiled in order and linked
together; a `.ASM` source is assembled with ASM80.

```bash
romwbw-plm80 com HELLO.PLM --tools PLM_WORK -o out
romwbw-plm80 prl DIR.PLM --tools PLM_WORK --tools UTIL9 --include UTIL8 -o out
romwbw-plm80 prl DM.PLM SN.PLM DSE.PLM DSO.PLM DSH.PLM DP.PLM DA.PLM DTS.PLM \
    --name SDIR --x0100 first --stack 50 --no-zero \
    --plm-options "DEBUG PAGEWIDTH(130)" --tools PLM_WORK --tools UTIL9 \
    --include UTIL8 -o out
romwbw-plm80 com GENSYS.PLM LDRLWR.ASM X0100.ASM --x0100 none \
    --plm-options DEBUG --tools PLM_WORK -o out
```

It exits 1 when PL/M-80 reports a program error, when a tool reports an error,
or when the output file is missing; the console log, kept with `--work DIR`,
says where.

## Results

Every PL/M program in `mpm2src` with a binary beside it, built from its source
with the recipe its own submit file uses, compared with DRI's binary as a
whole file - the PRL header and relocation bitmap included:

| Program | Source | Built as | Against DRI's |
|---|---|---|---|
| DIR, ERA, ERAQ, REN, TYPE | UTIL4 | prl | identical |
| SET, SHOW, STAT | UTIL4 | prl | identical (need `compact`) |
| ABORT, CONSOLE, DSKRESET, PRINTER, PRLCOM, SUBMIT, TOD, USER | UTIL5 | prl | identical |
| SCHED, SPOOL, MPMSTAT, STOPSPLR | UTIL5 | prl | identical |
| ED, PIP | UTIL6 | prl, `--data 1000` | identical (need `compact`) |
| GENSYS.COM | MPMLDR | com, with ASM80 | identical |
| MPMLDR.COM | MPMLDR | ISX, then MAC, PIP and LOAD | identical |
| SDIR | UTIL7 | prl, 8 modules | 526 bytes differ, all in uninitialised data |

**SDIR.** The differing bytes are 3A5AH-3C9DH of the file, after the last byte
any hex record sets. SDIR.SUB does not run ZERO before GENMOD, so those bytes
are whatever memory held - and in both files they are fragments of the
concatenated SDIR.HEX, left in memory by the PIP that made it. In DRI's file
they come from offsets in the hex text exactly 3072 bytes further on than in
ours. PIP buffers up to the BDOS, RomWBW's PIP.COM is byte for byte the PIP.COM
in `mpm2src/UTIL9`, and 3072 = E400H - D800H: DRI's PIP had a BDOS at E400H, a
62K CP/M 2.2, where ours is at D800H. The same machine would have had its BIOS
at F200H, so ISX gave its ISIS programs memory to F1FFH - which is where PIP's
compile, measured above, starts to fit.

MPMLDR.COM was built the whole way MPMLDR.SUB builds it - PL/M-80 and ASM80
under ISX, OBJHEX, then back in CP/M `MAC LDRBDOS`, `MAC LDRBIOS`,
`PIP MPMLDR.HEX=IMPMLDR.HEX[I],LDRBDOS.HEX[I],LDRBIOS.HEX[H]` and `LOAD
MPMLDR` - and it is identical, uninitialised bytes and all: an earlier build of
its PL/M part alone, outside ISX, differed from it in 71 of them.

**The distribution copies.** Six of these also ship in `mpm2dist`, and differ
there: SHOW.PRL (2 bytes, a constant 0EH that became 0FH), PRINTER.PRL (a jump
at 013FH to a patch at 02D5H), PIP.PRL (64 bytes), SPOOL.PRL (patch code
written over its "Enter ATTACH SPOOL" message), SDIR.PRL (a data size of
1000H in its header and one code byte) and GENSYS.COM (a different length). Those are DRI's later patches to the distributed binaries;
the source tree's binaries are the ones its sources build.

## Timing

Measured on a Mac whose six cores were shared with some fifty other runnable
processes, so wall clock was about four times CPU time; CPU time is the
figure to plan with.

| Build | Z80 instructions | CPU | wall, loaded |
|---|---|---|---|
| boot, one command, shut down | 0.7 M | 0.03 s | 0.1 s |
| DIFF1.PLM (174 lines): compile only | 21.8 M | 1.0 s | 3.7 s |
| DIFF1.PLM: the whole `com` route | 25.9 M | 1.1 s | 4.0 s |
| STAT.PLM (1,385 lines): compile only | 92.4 M | 4.2 s | 15.8 s |
| small `prl` builds (ABORT, CONSOLE, USER) | | 1.2-1.7 s | 6-10 s |
| ED, PIP | | 9-10 s | 57-63 s |
| SDIR, 8 modules | | 17 s | 99 s |

A small program is a second or two, which is quick enough for a test oracle.
The emulator stops a run at ten billion instructions; the whole rebuild above
is well under that per program, but a single batch compiling all of MP/M II
would come close.

## A differential test for uplm80

[uplm80](https://github.com/avwohl/uplm80) is a PL/M-80 compiler of this
family's own. With Intel's compiler runnable, the same source can be compiled
by both and the two programs' output compared. `tests/isx/DIFF1.PLM` is the
first such program: arithmetic on both widths with wrap-around, MOD and
division, LOW, HIGH, SHR, ROL, ROR, NOT, relations, SIZE and LAST, a DATA
table copied, summed and sorted, a BASED variable, DO CASE, a structure, and a
DO with BY; it prints 69 lines through BDOS functions 2 and 9.

```bash
romwbw-plm80 com tests/isx/DIFF1.PLM --tools PLM_WORK -o intel
PYTHONPATH=../uplm80:../upeepz80 python3 -m uplm80.compiler DIFF1.PLM -o DIFF1.MAC
um80 DIFF1.MAC && ul80 -o DIFF1.COM DIFF1.REL
cpmemu intel/DIFF1.COM > intel.out; cpmemu DIFF1.COM > uplm80.out
```

Both print the same 69 lines; the programs are 1,664 and 1,792 bytes.
`tests/batch_test.py` builds DIFF1 with Intel's compiler in both ISX modes when
`$ISX_TOOLS` names a `PLM_WORK` directory, and checks the two .COM files are
the same.

## Piped stdin

`romwbw-batch` does not type at the guest; it writes `$$$.SUB`. Typing at it
through a pipe works for a line or two and then loses keystrokes, and none of
the losses is the emulator's - a keyboard typed ahead loses them the same way:

- **RomWBW's boot loader reads what is waiting.** During the autoboot countdown
  - `--boot` sets one of zero seconds - `romldr.asm` reads keys until Enter or
  Esc and drops the rest, so the first line of piped input is gone before CP/M
  starts; at its prompt it flushes everything waiting. `printf 'STAT DSK:\rSTAT\r'
  | romwbw_emu --boot=2` runs only `STAT`.
- **CP/M programs read ahead.** The BDOS checks for ^S before each character it
  prints and keeps the key it finds; DDT abandons a `D` listing on any key;
  after a BDOS error, the next key is taken as the "any key".

Leading a pipe with an empty line gets past the boot loader. A submit file gets
past all of it.
