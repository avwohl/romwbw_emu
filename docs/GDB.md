# Debugging a guest program with GDB or udb80

romwbw_emu can wait for a debugger before the ROM runs the first instruction.
The debugger talks the GDB remote serial protocol (RSP) over a TCP socket on
127.0.0.1. GDB and udb80 (from um80_and_friends) both speak this protocol. The
`sim>` console of romwbw_emu keeps working at the same time. The design is in
`docs/debugger_plan.md` in the cpmemu repository. The stub itself comes from
cpmemu, and the protocol details are in `docs/gdb_stub.md` there.

## What you need

- **romwbw_emu 1.50 or newer.** An older release has no `--gdb`. 1.51 and
  newer also have `--gdb-listen`, and a `wait-program` stop that GDB shows.
  A build from source needs a cpmemu checkout beside it (see [Start](#start)).
- **um80 0.4.2 or newer:** `pip install "um80>=0.4.2"`, for `--debug-info`,
  udb80, `dbg2elf` and the GDB Python scripts. For a C program, also
  `pip install "uc80>=0.8.0"` and compile with `uc80 -g`.
- **GDB with the Z80 target**, but only if you use GDB. udb80 needs no GDB.

The list of what you need, with the GDB choices, is in
[What you need](https://github.com/avwohl/cpmemu/blob/main/docs/gdb_stub.md#what-you-need)
of the cpmemu document. The source-level steps are in the um80_and_friends
documents
[gdb.md](https://github.com/avwohl/um80_and_friends/blob/main/docs/gdb.md) and
[udb80.md](https://github.com/avwohl/um80_and_friends/blob/main/docs/udb80.md).

## Start

```
romwbw_emu --gdb ROM ...
romwbw_emu --gdb=4000 ROM ...
romwbw_emu --gdb=0 --gdb-port-file=port.txt ROM ...
romwbw_emu --gdb --gdb-listen=0.0.0.0 ROM ...
```

`--gdb` listens on 127.0.0.1 port 1234. `--gdb=PORT` chooses the port. Port 0
lets the system choose, and `--gdb-port-file=FILE` writes the chosen port to
FILE. romwbw_emu prints `GDB stub listening on 127.0.0.1:PORT` and waits for
the debugger. When the debugger connects, the CPU is at 0000H in ROM bank 00,
and no instruction has run. On a terminal, ^C ends romwbw_emu while the wait
lasts. `--gdb-port-file` without `--gdb` is an error: romwbw_emu prints
`--gdb-port-file needs --gdb` and exits with status 1.

## Debug from another machine

The stub listens on 127.0.0.1 by default. The stub has no password, and the
debugger can write guest memory. There are two ways to use a debugger on a
different machine:

- An SSH tunnel, the safer way. On the debugger machine, run
  `ssh -N -L 1234:127.0.0.1:1234 USER@EMULATOR-HOST`, then connect the
  debugger to port 1234 on the debugger machine.
- `--gdb-listen=ADDR` puts the stub on the IPv4 address ADDR: `0.0.0.0` for
  every interface, or the address of one interface. The option needs
  `--gdb`. A bad address gives `Invalid --gdb-listen address` and exit
  status 1. For an address outside 127.0.0.0/8, romwbw_emu prints a warning.
  Then connect with `target remote EMULATOR-HOST:1234`, or with
  `udb80 PROG.dbg --connect EMULATOR-HOST:1234 --wait-program`. Use the
  option only on a network that you trust.

The cpmemu document `docs/gdb_stub.md` (Debug from another machine) has the
reason and the commands in full. `tests/gdb_stub_test.py` checks the
option. Bad addresses are refused. Without the option, the LAN address is
refused. With `0.0.0.0`, a client on the LAN address connects. With the
address of one interface, 127.0.0.1 is refused. Measured on 2026-11-05 (macOS 27 arm64)
with the LAN address of the Mac. A debugger on a different machine was not
measured.

The stub is off by default. A romwbw_emu that runs without `--gdb` runs the
same loop as before (see [Speed](#speed)).

`make` compiles `gdbstub.cc` from the source directory of cpmemu. `make` looks in the
directory that `QKZ80_CFLAGS` names with `-I`, then in `../../cpmemu/src`.
`make CPMEMU_SRC=DIR` sets the directory. When make finds no `gdbstub.cc`, it
builds romwbw_emu without the stub, and `--gdb` prints an error and exits.

## The ROM, the banks, and 16-bit addresses

A GDB address for the Z80 has 16 bits. The Z80 in romwbw_emu sees 64 KB: a
bank in the low 32 KB, and the common RAM bank 8FH from 8000H up.

- `g`, `m`, `M`, `X`, `Z0` and `Z1` use the mapping that the CPU has now. A
  breakpoint at 0200H stops at 0200H in whichever bank the CPU has at that
  moment.
- `monitor bank XX` makes the debugger see bank XX in the low 32 KB, without
  a change to what the CPU sees. XX is 00 to 0F (ROM) or 80 to 8F (RAM).
  Addresses from 8000H up are always bank 8FH. `monitor bank cur` goes back to
  the bank that the CPU has. A write to a ROM bank has no effect.
- `monitor break-bank XX` makes the breakpoints below 8000H apply only when the
  CPU is in bank XX. `monitor break-bank off` removes the limit.
- `monitor status` shows the PC, the CPU bank, the view, the break bank, and
  the wait-program rule.

The stub reads and writes guest memory through `banked_mem::peek_mem()` and
`store_mem()`. A debugger read sets no trace bit.

## Find a program that CP/M starts: wait-program

romwbw_emu does not load a CP/M `.COM` file. CP/M loads the file from a disk
image into bank 8EH at 0100H, and the CCP jumps to 0100H. The debugger
therefore cannot start the program. The debugger asks the stub to stop when
the program starts.

```
monitor wait-program bytes 0E 09 11 1E 01 CD 05 00
monitor wait-program sha256 128 4f6748eb...(64 hex digits)
monitor wait-program off
monitor wait-program
```

- `bytes HEX...` gives the first bytes of the `.COM` file (1 to 4096 bytes).
- `sha256 LENGTH HEX64` gives the length of the program in decimal and the
  SHA-256 of those bytes. A `prog.dbg` file from `ul80 --debug-info` has both
  values in `image.length` and `image.sha256`.
- The stub stops when PC is 0100H, the CPU is in bank 8EH, and the bytes at
  0100H match. The stop reply is the packet `T05`, with no `swbreak`, because
  no debugger set a breakpoint there. GDB reports the stop as a SIGTRAP.
  (Before 2026-11-05 the reply was `T05swbreak:;`, and GDB did not stop.)
- The rule fires once. Then the stub does three things. The stub sets the break
  bank to 8EH. The stub restarts the shadow call stack with one frame, the
  return address at SP, which is the address in the CCP. The stub prints a line
  to standard error.
- A program that does not match never stops the run. The run goes on to its end.

A normal session:

1. Start `romwbw_emu --gdb=0 --gdb-port-file=port.txt ...` with a disk image
   that holds the `.COM` file. Give the console the command line that starts
   the program (`printf 'PROG\r' | romwbw_emu ...`, or type the name).
2. Connect the debugger.
3. Send `monitor wait-program sha256 LENGTH HEX64`, and then `continue`.
   udb80 `--wait-program` does this step. With GDB, use the steps in
   [Find the program with GDB](#find-the-program-with-gdb).
4. The stop at 0100H is the start of the program. Set breakpoints, and go on.

The rule is for CP/M 2.2 style systems, where the program is in bank 8EH at
0100H. Other systems (CP/M 3, ZPM3) load programs differently, and the rule
does not find them.

### Find the program with GDB

Type these commands after `target remote`, before CP/M starts the program:

```
(gdb) monitor wait-program sha256 LENGTH HEX64
(gdb) continue
```

Then start the program at the CP/M prompt. GDB stops with
`Program received signal SIGTRAP, Trace/breakpoint trap.` and the first line
of the program, at 0100H in bank 8EH. The rule has set the break bank to 8EH.
Now set the breakpoints of the program, for example `break inner`, and type
`continue`.

This needs romwbw_emu 1.51 or later, or a build with cpmemu `365dfc2` or
later. An older stub, as in 1.50,
replies `T05swbreak` for the rule's stop. GDB set no breakpoint at 0100H, so
GDB continues past that stop with no message, and the program runs to its end.
With an older stub, use a breakpoint of GDB's own:

```
(gdb) monitor break-bank 8E
(gdb) break *0x100
(gdb) continue
```

These commands stop at the first program that CP/M starts at 0100H, whatever
its bytes are. GDB's breakpoint stops the program before the stub tests the
rule.
Without `monitor break-bank 8E`, GDB stops at 0100H during the boot of the
ROM, in bank 01, and shows the first line of the program for that wrong stop.

Measured on 2026-11-05 with GDB 18.1, the ELF file of `tests/gdb_prog.asm`
from `dbg2elf`, and the 3.6.0 ROM with `hd1k_cpm22`:

- New stub, `monitor wait-program` and `continue`: SIGTRAP at `start ()`,
  0100H, bank 8EH. `break inner`, `continue` and `bt` showed `inner`,
  `outer` and `start`. The same with `set remote swbreak-feature-packet off`.
- Old stub, `monitor wait-program` and `continue`: the rule fired, and GDB
  did not stop.
- Old stub, `break *0x100` without `break-bank`: GDB stopped at 0100H with
  the CPU in bank 01.
- Both stubs, `monitor break-bank 8E`, `break *0x100` and `continue`:
  `Breakpoint 1` at 0100H in bank 8EH, then `break inner` and `bt` worked.

## udb80

udb80 is in the um80 package (`pip install "um80>=0.4.2"`). Connect with
`--wait-program`:

```
udb80 PROG.dbg --connect 127.0.0.1:PORT --wait-program
```

At connect time, udb80 reads guest memory at 0100H and compares the memory
with `prog.dbg`. When udb80 connects, the ROM has not started yet. Thus the
memory does not hold the program, and without `--wait-program` udb80 refuses
the file. `--wait-program` does these steps before the memory check:

1. Send `monitor wait-program sha256 LENGTH HEX64` with the values from
   `prog.dbg`.
2. Send `c` and wait for the stop reply at 0100H: `T05` from a stub of
   2026-11-05 or later, `T05swbreak` from an older stub. udb80 0.4.1 takes
   only `T05swbreak`, so use udb80 0.4.2 or later with a newer stub.
3. Run the memory check, and go on as for cpmemu.

A session with `--wait-program` (measured on 2026-11-04 with a copy of
`udb80.py` before the option went into um80 0.4.0, `bt` output shortened):

```
Breakpoint 1, INNER () at gprog.asm:13
#0  INNER () at gprog.asm:13  [shadow]
#1  0x0119 in OUTER () at gprog.asm:11  [shadow]
#2  0x010b in START () at gprog.asm:6  [shadow]
```

`break`, `continue`, `bt`, `info registers`, `stepi`, `finish` and `monitor`
worked. For GDB, see [Find the program with GDB](#find-the-program-with-gdb).

## monitor bt

`monitor bt` prints the shadow call stack that the stub keeps.

After a call to BDOS, the output ends with `(the shadow stack is out of step...)`.
The reason is in the CP/M 2.2 CBIOS of the 3.6.0 ROM: the CBIOS pushes a return address and uses `RET` as a jump
(measured: a `RET` at E791H goes to E77CH with no `CALL`). The frames of the
program stay right.

## Console, ^C and sim>

- ^C from the debugger (the byte 03H) stops the guest and answers `T02`. This
  works while the guest waits for a key. A wait for a key makes the console
  read give the turn back to the stub every 10 ms. Measured: the `T02` arrives
  in under 2 s.
- ^E on a terminal still opens `sim>`. A `sim>` breakpoint (`bp`) still opens
  `sim>`. The `q` command in `sim>` ends the run, and the debugger receives `W00`.
- A HLT stops the guest with `T05`. The debugger can go on past the HLT.
  Without a debugger, a HLT ends the run.
- `D` lets the program run on with no debugger. `k` ends romwbw_emu with
  status 1. The instruction limit gives `W7c` (status 124).

## Speed

The loop without `--gdb` is the old loop. The plain loop and the stub share four
lambdas. `ROMWBW_TURN_INLINE` makes the compiler put the lambdas into the loop.
Measured on 2026-11-04. The test is a DEC/OR/JR loop of 200 million
instructions under CP/M, and the numbers are median user times. The machine had
a load average of 12. Over 16 interleaved runs, the old binary took 5.81 s and
the new binary took 5.89 s, which is 1.4 percent more.

An earlier set of 10 runs on a quieter machine gave 3.52 s and 3.53 s. Without
the attribute, the new binary took 8.57 s against 5.91 s (5 runs), which is 45
percent more.

## Limits

- No watchpoints: `Z2` to `Z4` get an empty reply. There is no `QCatchSyscalls`
  either, because BDOS calls here are code in RAM and not traps.
- A breakpoint below 8000H applies in every bank until the rule or
  `monitor break-bank` limits the bank.
- One debugger at a time. The stub is on 127.0.0.1 unless `--gdb-listen`
  names a different address (see
  [Debug from another machine](#debug-from-another-machine)).
- `monitor wait-program` knows the bank 8EH and 0100H only.

## Other clients

ioscpm, z80cpmw and cpmdroid compile `hbios_dispatch.cc`, `hbios_cpu.cc`
and `romwbw_mem.h` from this tree, and each has its own run loop. Since
2026-11-05 each one also has the stub, off by default, with a setting for the
port and a setting for the network. Each one's `docs/gdb.md` says how to use
it:

- ioscpm (iOS, Mac):
  [docs/gdb.md](https://github.com/avwohl/ioscpm/blob/main/docs/gdb.md)
- z80cpmw (Windows):
  [docs/gdb.md](https://github.com/avwohl/z80cpmw/blob/master/docs/gdb.md)
- cpmdroid (Android):
  [docs/gdb.md](https://github.com/avwohl/cpmdroid/blob/master/docs/gdb.md)

The z80cpmw and cpmdroid builds were not made on a Windows or Android
machine. Each one's `todo.txt` says what is still to verify. The debugger plan
in cpmemu (`docs/debugger_plan.md`, Phase 9 results) has the measurements.

The parts that a client needs:

- `gdbstub.cc` from cpmemu, and `romwbw_gdb.cc` from this tree. Both are
  portable C++11.
- The socket functions of `gdb_platform.cc`. `gdb_platform.cc` uses POSIX calls and is usable
  on macOS, iOS and Android. A Windows client needs the Winsock
  functions that cpmemu has in `src/os/windows/platform.cc`.
- A turn function with the order of the lambdas in `main()` of `romwbw_emu.cc`:
  interrupts of the last turn, the instruction, what follows the instruction.
  The `step_fn` member of `RomwbwTarget` calls this function.
- A thread. `Stub::serve()` does not return while the debugger holds the program. A client must
  call `serve()` on its emulation thread and not on its UI thread.
  z80cpmw calls `runBatch` from `onTimer` on the UI thread.
- A setting that turns the stub on and chooses the port. The stub is off by
  default.
- A platform layer. `gdb_platform.cc` waits in `accept()` and `recv()` with
  no time limit. ioscpm and z80cpmw have their own socket layers that wait in
  100 ms slices and look at a cancel flag. cpmdroid uses `gdb_platform.cc` and
  ends a wait with a signal to the stub thread.
