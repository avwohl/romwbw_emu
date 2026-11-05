# Debugging a guest program with GDB or udb80

romwbw_emu can wait for a debugger before the ROM runs the first instruction.
The debugger talks the GDB remote serial protocol (RSP) over a TCP socket on
127.0.0.1. GDB and udb80 (from um80_and_friends) both speak this protocol. The
`sim>` console of romwbw_emu keeps working at the same time. The design is in
`docs/debugger_plan.md` in the cpmemu repository. The stub itself comes from
cpmemu, and the protocol details are in `docs/gdb_stub.md` there.

## Start

```
romwbw_emu --gdb ROM ...
romwbw_emu --gdb=4000 ROM ...
romwbw_emu --gdb=0 --gdb-port-file=port.txt ROM ...
```

`--gdb` listens on 127.0.0.1 port 1234. `--gdb=PORT` chooses the port. Port 0
lets the system choose, and `--gdb-port-file=FILE` writes the chosen port to
FILE. romwbw_emu prints `GDB stub listening on 127.0.0.1:PORT` and waits for
the debugger. When the debugger connects, the CPU is at 0000H in ROM bank 00,
and no instruction has run. On a terminal, ^C ends romwbw_emu while the wait
lasts. `--gdb-port-file` without `--gdb` is an error: romwbw_emu prints
`--gdb-port-file needs --gdb` and exits with status 1.

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
  0100H match. The stop reply is the packet `T05swbreak:;`.
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
4. The stop at 0100H is the start of the program. Set breakpoints, and go on.

The rule is for CP/M 2.2 style systems, where the program is in bank 8EH at
0100H. Other systems (CP/M 3, ZPM3) load programs differently, and the rule
does not find them.

## udb80

udb80 connects with `udb80 PROG.dbg --connect 127.0.0.1:PORT`. The udb80 of
um80_and_friends (commit 95555dd) cannot attach before the program starts. At
connect time, udb80 reads guest memory at 0100H and compares the memory with
`prog.dbg`. The memory holds no program yet, so udb80 refuses the file.

The needed change is small. An option `--wait-program` does these steps before
the memory check:

1. Send `monitor wait-program sha256 LENGTH HEX64` with the values from
   `prog.dbg`.
2. Send `c` and wait for the stop reply `T05swbreak`.
3. Run the memory check, and go on as for cpmemu.

A session with a copy of `udb80.py` that has this option (measured on
2026-11-04, `bt` output shortened):

```
Breakpoint 1, INNER () at gprog.asm:13
#0  INNER () at gprog.asm:13  [shadow]
#1  0x0119 in OUTER () at gprog.asm:11  [shadow]
#2  0x010b in START () at gprog.asm:6  [shadow]
```

`break`, `continue`, `bt`, `info registers`, `stepi`, `finish` and `monitor`
worked. The option is not yet in um80_and_friends. GDB needs no change: send
the `monitor wait-program` command from the GDB prompt, and use `continue`.

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
- One debugger, on 127.0.0.1 only.
- `monitor wait-program` knows the bank 8EH and 0100H only.

## Other clients

ioscpm, z80cpmw and cpmdroid compile `hbios_dispatch.cc`, `hbios_cpu.cc`
and `romwbw_mem.h` from this tree, and each has its own run loop. None of them
has the stub yet. A client needs these parts:

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

The work items are in the `todo.txt` of each client.
