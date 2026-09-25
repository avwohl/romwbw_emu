#!/usr/bin/env python3
"""batch_test.py - tools/romwbw-batch and tools/romwbw-plm80.

Three layers, each skipped when what it needs is not here:

  * the pieces, with no emulator: the $$$.SUB layout DRI's SUBMIT.COM writes,
    the Intel OMF the ISX return module is made of, the compact ISX BIOS the
    tool carries against tools/isxbios.asm assembled now (needs um80/ul80),
    and ISX's exact-length convention on a disk image (needs cpm_disk.py);
  * batches on the emulator (needs src/romwbw_emu and a cached ROM and
    hd1k_cpm22 - `tools/romwbw-get fetch @rom hd1k_cpm22`): a batch that runs
    to its end, one that does not, and the two programs that found the
    console idle bug - PIP concatenating files and MBASIC running a loop, both
    of which the emulator used to cut off at end of input;
  * Intel PL/M-80 under ISX (needs $ISX_TOOLS naming DRI's PLM_WORK
    directory, which this repository does not carry): a program compiled by
    both ISX modes, and DRI's `CPM` getting back to CP/M from each.

Run: python3 tests/batch_test.py      (from anywhere)
     make -C src test
"""

import importlib.machinery
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOLS = os.path.join(ROOT, "tools")

failures = []


def check(ok, what):
    print(("PASS" if ok else "FAIL") + ": " + what)
    if not ok:
        failures.append(what)


def skip(what):
    print("SKIP: " + what)


def load(name, path):
    loader = importlib.machinery.SourceFileLoader(name, path)
    spec = importlib.util.spec_from_loader(name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


rb = load("romwbw_batch", os.path.join(TOOLS, "romwbw-batch"))


# --- the pieces ------------------------------------------------------------------

def test_submit_layout():
    data = rb.submit_file(["DIR", "STAT *.SUB"])
    check(len(data) == 256, "$$$.SUB: one 128-byte record per command")
    check(data[0:12] == b"\x0aSTAT *.SUB\x00" and data[12:13] == b"$",
          "$$$.SUB: the LAST command first, as length, text, 00, '$' - the "
          "layout SUBMIT.COM wrote when dumped")
    check(data[128:134] == b"\x03DIR\x00$", "$$$.SUB: then the first command")
    try:
        rb.submit_file(["X" * 126])
        check(False, "a command longer than the CCP's buffer is refused")
    except rb.UsageError:
        check(True, "a command longer than the CCP's buffer is refused")


def omf_records(b):
    recs, p = [], 0
    while p < len(b):
        t, n = b[p], b[p + 1] | b[p + 2] << 8
        body = b[p:p + 3 + n]
        recs.append((t, body[3:-1], sum(body) & 0xFF))
        p += 3 + n
    return recs


class FakeSystem(object):
    """A CP/M 2.2 image laid out as RomWBW's: CCP D000H, BDOS D800H, BIOS E600H."""
    load, end, entry = 0xD000, 0xFE00, 0xE600

    def __init__(self):
        img = bytearray(self.end - self.load)
        for i in range(0x800, 0x1600):
            img[i] = (i * 7) & 0xFF
        img[0x1601:0x1603] = b"\x34\x12"     # the BIOS cold-boot vector
        self.image = bytes(img)

    mem = rb.SystemImage.mem
    cpm22_layout = rb.SystemImage.cpm22_layout


def test_return_module():
    sysimg = FakeSystem()
    mod = rb.isx_return_module(sysimg)
    recs = omf_records(mod)
    check(all(c == 0 for _, _, c in recs), "return module: every OMF record "
                                           "checksums to zero")
    check([t for t, _, _ in recs][0] == 0x02 and recs[-2][0] == 0x04 and
          recs[-1][0] == 0x0E, "return module: header ... end, EOF")
    mem = bytearray(0x10000)
    for t, body, _ in recs:
        if t == 0x06:
            off = body[1] | body[2] << 8
            mem[off:off + len(body) - 3] = body[3:]
    start = recs[-2][1][2] | recs[-2][1][3] << 8
    check(start == 0x3680, "return module: starts where ISIS loads, 3680H")
    bdos = bytes(mem[0x36C0:0x36C0 + rb.BDOS_SIZE])
    check(bdos == sysimg.image[0x800:0x1600],
          "return module: carries the BDOS from the system image")
    check(b"\x21\x34\x12\x22\x01\xE6" in bytes(mem[0x3680:0x36C0]),
          "return module: puts back the BIOS cold-boot vector ISX patched")


def fake_isx():
    img = bytearray(rb.ISX_SIZE)
    img[0:3] = b"\xC3\x00\x30"
    img[0x1A20:0x1A20 + len(rb.ISX_SIGNATURE)] = rb.ISX_SIGNATURE
    return bytes(img)


def test_isx_compact():
    isx = fake_isx()
    check(rb.is_isx14(isx), "ISX 1.4 is recognised by its banner")
    check(not rb.is_isx14(isx[:100]), "...and a short file is not ISX")
    w = rb.isx_compact(isx, 2, 0, 0)
    check(w[0:3] == b"\xC3\x00\x32", "compact: ISX now starts at 3200H")
    check(w[3:rb.ISX_SIZE] == isx[3:], "compact: the rest of ISX is untouched")
    tail = w[rb.ISX_SIZE:]
    check(tail.endswith(rb.ISXBIOS[rb.ISXBIOS_BANK + 1:]),
          "compact: the BIOS image follows the installer")
    blob_at = len(tail) - len(rb.ISXBIOS)
    bios = tail[blob_at:]
    check(bios[rb.ISXBIOS_NDRIVE] == 2 and bios[rb.ISXBIOS_UNITS:
                                                rb.ISXBIOS_UNITS + 2] == b"\x02\x03",
          "compact: two drives, HBIOS units 2 and 3")
    check(b"\x21\x03\xF6\x22\x01\x00" in tail[:blob_at],
          "compact: the installer points 0001H at the BIOS warm boot, F603H")
    check(b"\x21\x00\x30\x22\x01\x01\xC3\x00\x01" in tail[:blob_at],
          "compact: ...and puts ISX's JMP 3000H back before jumping to it")
    check(rb.returns_from_isx(":f1:cpm") and rb.returns_from_isx("CPM") and
          not rb.returns_from_isx("CPMX"), "`CPM` is recognised however spelt")


def test_isxbios_source():
    um80, ul80 = shutil.which("um80"), shutil.which("ul80")
    if not (um80 and ul80):
        skip("isxbios.asm against the carried bytes: um80/ul80 not on PATH "
             "(pip install um80)")
        return
    with tempfile.TemporaryDirectory() as d:
        shutil.copy(os.path.join(TOOLS, "isxbios.asm"), d)
        r1 = subprocess.run([um80, "isxbios.asm", "-o", "isxbios.rel"], cwd=d,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        r2 = subprocess.run([ul80, "-o", "isxbios.bin", "-p", "F600",
                             "isxbios.rel"], cwd=d,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        ok = r1.returncode == 0 and r2.returncode == 0
        got = open(os.path.join(d, "isxbios.bin"), "rb").read() if ok else b""
    check(ok and got[:len(rb.ISXBIOS)] == rb.ISXBIOS and
          not got[len(rb.ISXBIOS):].strip(b"\x00"),
          "tools/isxbios.asm assembles to the bytes romwbw-batch carries")
    check(rb.ISXBIOS_ORG + len(rb.ISXBIOS) <= rb.ISX_MONITOR_LO,
          "the compact BIOS ends below ISX's monitor vector at F800H")


def test_exact_lengths(cd):
    with tempfile.TemporaryDirectory() as d:
        img = os.path.join(d, "t.img")
        with open(img, "wb") as f:
            f.write(cd.create_hd1k_disk())
        drv = rb.Drive(cd, img, exact=True)
        drv.add("T.PLM", rb.pad_ctrl_z(b"X" * 435), 435)
        drv.add("B.BIN", b"\x01" * 200)
        drv.save()
        again = rb.Drive(cd, img, exact=True)
        check(again.read("T.PLM") == b"X" * 435,
              "exact: a 435-byte text file comes back 435 bytes, not 512")
        off = again._last_extent("T.PLM")
        check(again.data[off + 13] == 77,
              "exact: S1 holds the 77 unused bytes of the last record, as "
              "ISX writes it")
        check(again.read("B.BIN") == b"\x01" * 200,
              "exact: a binary file keeps its length too")
        plain = rb.Drive(cd, img, exact=False)
        check(len(plain.read("T.PLM")) == 512,
              "without --exact a file is whole records, as CP/M sees it")
    check(rb.text_bytes(b"a\nb\r\nc\x1a\x1ajunk") == b"a\r\nb\r\nc",
          "text: LF becomes CR LF, and the file ends at its first ^Z")


# --- batches on the emulator ----------------------------------------------------

def run_tool(argv, cwd, timeout=300):
    return subprocess.run([sys.executable] + argv, cwd=cwd, timeout=timeout,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          universal_newlines=True)


def batch(args, cwd, timeout=300):
    return run_tool([os.path.join(TOOLS, "romwbw-batch"), "--offline"] + args,
                    cwd, timeout)


def test_batches():
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "hi.txt"), "w") as f:
            f.write("hello\nworld\n")
        r = batch(["-t", "hi.txt", "-c", "TYPE HI.TXT", "-c", "PIP HI2.TXT=HI.TXT",
                   "-g", "HI2.TXT", "-o", "out", "--work", "w"], d)
        check(r.returncode == 0, "a two-command batch completes (exit 0)")
        got = open(os.path.join(d, "out", "HI2.TXT"), "rb").read() \
            if os.path.exists(os.path.join(d, "out", "HI2.TXT")) else b""
        check(got.startswith(b"hello\r\nworld\r\n\x1a"),
              "the file PIP wrote comes back out")
        log = open(os.path.join(d, "w", "console.log"), encoding="latin-1").read()
        check("A>TYPE HI.TXT" in log and "hello" in log,
              "the console log holds each command and its output")

        r = batch(["-c", "NOSUCH", "-c", "DIR"], d)
        check(r.returncode == 1 and "did not complete" in r.stdout,
              "a command the CCP cannot find stops the batch, and it says so")

        # The console idle bug.  PIP polls for a key between records and did
        # no disk I/O for eight of them, the emulator took that for a guest
        # waiting at a prompt, and with stdin at end of file it ended the run
        # with PIP's output a zero-length temporary file.
        with open(os.path.join(d, "a.txt"), "w") as f:
            f.write("".join(":%04X\n" % i for i in range(300)))
        with open(os.path.join(d, "b.txt"), "w") as f:
            f.write("".join(":%04X\n" % (i + 300) for i in range(300)))
        r = batch(["-t", "a.txt", "-t", "b.txt", "-c", "PIP C.TXT=A.TXT,B.TXT",
                   "-g", "C.TXT", "-o", "out"], d)
        cat = open(os.path.join(d, "out", "C.TXT"), "rb").read() \
            if os.path.exists(os.path.join(d, "out", "C.TXT")) else b""
        check(r.returncode == 0 and cat.startswith(
            "".join(":%04X\r\n" % i for i in range(600)).encode()),
            "PIP concatenating two files is not cut off at end of input")

        # ...and MBASIC, which checks for ^C before every statement, ran at a
        # hundred instructions a second and never printed DONE.
        with open(os.path.join(d, "loop.bas"), "w") as f:
            f.write('10 FOR I=1 TO 3000: X=X+1: NEXT\n20 PRINT "DONE"\n'
                    '30 SYSTEM\n')
        r = batch(["-t", "loop.bas", "-c", "MBASIC LOOP", "--timeout", "60",
                   "--work", "m"], d)
        log = open(os.path.join(d, "m", "console.log"), encoding="latin-1").read() \
            if os.path.exists(os.path.join(d, "m", "console.log")) else ""
        check(r.returncode == 0 and "DONE" in log,
              "MBASIC running a loop finishes and prints DONE")


# --- Intel PL/M-80 under ISX -------------------------------------------------------

def test_isx(tools_dir):
    src = os.path.join(HERE, "isx", "DIFF1.PLM")
    for mode in ("compact", "cbios"):
        with tempfile.TemporaryDirectory() as d:
            if mode == "compact":
                r = run_tool([os.path.join(TOOLS, "romwbw-plm80"), "com", src,
                              "--tools", tools_dir, "--offline", "-o", "out",
                              "--work", "w"], d, timeout=600)
            else:
                # The recipe's own route, through romwbw-batch by hand, with the
                # tools on A: as DRI's UTIL submit files had them.
                r = batch(["--isx=cbios", "-a", tools_dir, "-t", src,
                           "-c", "ISX", "-c", "PLM80 DIFF1.PLM",
                           "-c", "LINK DIFF1.OBJ,X0100,PLM80.LIB TO DIFF1.MOD",
                           "-c", "LOCATE DIFF1.MOD CODE(0100H) STACKSIZE(100)",
                           "-c", "CPM", "-c", "OBJCPM DIFF1",
                           "-g", "DIFF1.COM", "-g", "DIFF1.LST", "-o", "out",
                           "--work", "w"], d, timeout=600)
            com = os.path.join(d, "out", "DIFF1.COM")
            lst = os.path.join(d, "out", "DIFF1.LST")
            check(r.returncode == 0 and os.path.exists(com),
                  "ISX (%s): PL/M-80 compiles, links and locates DIFF1, and "
                  "OBJCPM makes DIFF1.COM back in CP/M" % mode)
            text = open(lst, "rb").read().decode("latin-1") \
                if os.path.exists(lst) else ""
            check("0 PROGRAM ERROR" in text,
                  "ISX (%s): the listing reports no errors" % mode)
            built = open(com, "rb").read() if os.path.exists(com) else None
            if mode == "compact":
                first = built
            else:
                check(built is not None and built == first,
                      "ISX: both modes build the same DIFF1.COM")


def main():
    print("romwbw-batch and romwbw-plm80")
    print("------------------------------------------------------------")
    test_submit_layout()
    test_return_module()
    test_isx_compact()
    test_isxbios_source()
    try:
        cd = rb.find_cpm_disk()
    except rb.BatchError:
        cd = None
        skip("exact lengths on an image: cpm_disk.py not found")
    if cd:
        test_exact_lengths(cd)

    ready = cd is not None
    why = ""
    try:
        rb.find_emulator()
        rb.resolve_asset("@rom", offline=True)
        rb.resolve_asset(rb.DEFAULT_SYSTEM_DISK, offline=True)
    except rb.BatchError as e:
        ready, why = False, str(e).splitlines()[0]
    if not ready:
        skip("batches on the emulator: " + (why or "cpm_disk.py not found"))
    else:
        test_batches()
        tools_dir = os.environ.get("ISX_TOOLS", "").split(os.pathsep)[0]
        if tools_dir and os.path.isfile(os.path.join(tools_dir, "ISX.COM")):
            test_isx(tools_dir)
        else:
            skip("Intel PL/M-80 under ISX: set $ISX_TOOLS to DRI's PLM_WORK "
                 "directory (mpm2src/PLM_WORK)")

    print("------------------------------------------------------------")
    if failures:
        print("%d FAILED" % len(failures))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
