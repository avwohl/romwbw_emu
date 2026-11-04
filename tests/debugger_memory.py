#!/usr/bin/env python3
"""debugger_memory.py - the sim> console's e, dm and d read and write the
memory the CPU sees.

The three commands used to index the flat 64K array of qkz80_cpu_mem.  In
banked mode banked_mem keeps the guest's memory in its own ram[] and rom[], so
`dm FFF0` on a booted ROM printed 16 zeros and `d` wrote where no instruction
ever ran.  This test boots the cached ROM on a pty, presses ^E at the boot
loader's prompt and checks:

  * dm at FFF0 shows the HBIOS entry stub, D3 EF C9 (OUT (0EFh),A ; RET),
    which is in the common RAM bank;
  * dm at 0000 is not all zeros: it is the ROM bank's reset vector;
  * d at 9000 of 3E 5A (LD A,5Ah), then pc 9000 and one step, leaves A=5A, so
    the CPU ran what d deposited, and e reads the same bytes back.

Skipped when src/romwbw_emu, a cached ROM or a pty is missing.

Run: python3 tests/debugger_memory.py      (from anywhere)
     make -C src test
"""

import importlib.machinery
import importlib.util
import os
import re
import select
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

failures = []


def check(ok, what):
    print(("PASS" if ok else "FAIL") + ": " + what)
    if not ok:
        failures.append(what)


def read_for(fd, seconds):
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if not r:
            continue
        try:
            data = os.read(fd, 65536)
        except OSError:
            break
        if not data:
            break
        out += data
    return out.decode("latin1").replace("\r", "")


def main():
    try:
        import pty
    except ImportError:
        print("SKIP: debugger memory: no pty module here")
        return 0
    emu = os.path.join(ROOT, "src", "romwbw_emu")
    if not os.access(emu, os.X_OK):
        print("SKIP: debugger memory: src/romwbw_emu is not built")
        return 0
    try:
        batch = importlib.machinery.SourceFileLoader(
            "romwbw_batch", os.path.join(ROOT, "tools", "romwbw-batch"))
        bspec = importlib.util.spec_from_loader("romwbw_batch", batch)
        rbm = importlib.util.module_from_spec(bspec)
        batch.exec_module(rbm)
        rom = rbm.resolve_asset("@rom", offline=True)
    except BaseException as e:  # SystemExit from a tool means "not cached"
        print("SKIP: debugger memory: no cached ROM (%s)" % (e,))
        return 0

    pid, fd = pty.fork()
    if pid == 0:
        os.execv(emu, [emu, rom])
    out = read_for(fd, 2.5)
    os.write(fd, b"\x05")
    out = read_for(fd, 1.0)
    if "sim> " not in out:
        check(False, "^E at the boot loader's prompt reached sim>")
        return 1

    def cmd(line, wait=0.5):
        os.write(fd, line.encode() + b"\n")
        return read_for(fd, wait)

    dm_stub = cmd("dm FFF0 3")
    check("FFF0: D3 EF C9" in dm_stub,
          "dm FFF0 shows the HBIOS entry stub D3 EF C9 (got %r)" % dm_stub)
    dm_rom = cmd("dm 0 8")
    check(re.search(r"0000: (?!00 00 00 00 00 00 00 00)", dm_rom) is not None,
          "dm 0 shows the ROM's reset vector, not zeros (got %r)" % dm_rom)
    e_stub = cmd("e FFF0 3")
    check("FFF0: D3" in e_stub and "FFF2: C9" in e_stub,
          "e FFF0 3 shows D3 EF C9 (got %r)" % e_stub)
    cmd("d 9000 3e 5a")
    e_dep = cmd("e 9000 2")
    check("9000: 3E" in e_dep and "9001: 5A" in e_dep,
          "e reads back what d deposited at 9000 (got %r)" % e_dep)
    cmd("pc 9000")
    cmd("s")
    regs = cmd("r")
    check("A=5A" in regs,
          "the CPU ran the deposited LD A,5Ah: A=5A (got %r)" % regs)
    os.write(fd, b"q\n")
    read_for(fd, 0.5)
    try:
        os.waitpid(pid, 0)
    except OSError:
        pass

    if failures:
        print("FAILED: %d check(s)" % len(failures))
        return 1
    print("ALL debugger memory checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
