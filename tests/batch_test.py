#!/usr/bin/env python3
"""batch_test.py - tools/romwbw-batch and tools/romwbw-plm80.

Three layers, each skipped when what it needs is not here:

  * the pieces, with no emulator: the $$$.SUB layout DRI's SUBMIT.COM writes,
    the Intel OMF the ISX return module is made of, the compact ISX BIOS the
    tool carries against tools/isxbios.asm assembled now (needs um80/ul80),
    ISX's exact-length convention on a disk image (needs cpm_disk.py), and
    romwbw-plm80's reading of the console and its handling of the -o
    directory, which reported a failed rebuild as built because the last
    build's output was still there, and of the system disk's own files, one
    of which it took for a failed build's output, and both tools refusing a
    bad argument before they resolve the ROM;
  * batches on the emulator (needs src/romwbw_emu and a cached ROM and
    hd1k_cpm22 - `tools/romwbw-get fetch @rom hd1k_cpm22`): a batch that runs
    to its end, one that does not, -g of a system disk file the batch left
    alone, which came back with no word said, and the two programs that found
    the console idle bug - PIP concatenating files and MBASIC running a loop,
    both of which the emulator used to cut off at end of input - and piped
    stdin reaching the CCP and the boot menu, which the boot loader used to
    eat, and a script's leading Esc still stopping the autoboot countdown,
    which the hold on piped input took away, and the instruction limit, which
    used to end a run with exit status 0, and a program booted from the disk
    that prints between polls and never waits, which the hold on piped input
    starved; and, when hd1k_combo is cached too, ZPM3, whose prompt the idle
    rule missed so that the emulator never ended there and held piped input
    from it for ever, and CP/M 3 and NZ-COM, whose starts ate a script's first
    key when the hold let go at the boot loader's handover; and on a pty, ^E
    at the boot loader's prompt, which took a tenth of a second or more to
    reach sim> while the CLI looked for it only every 1024th instruction;
  * Intel PL/M-80 under ISX (needs $ISX_TOOLS naming DRI's PLM_WORK
    directory, which this repository does not carry): a program compiled by
    both ISX modes, and DRI's `CPM` getting back to CP/M from each; a rebuild
    that fails reported as failed; a build named ED, as the system disk's
    editor is, that fails and one that does not; and --isx=compact with
    nothing on B:.

Run: python3 tests/batch_test.py      (from anywhere)
     make -C src test
"""

import argparse
import contextlib
import importlib.machinery
import importlib.util
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

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
rp = load("romwbw_plm80", os.path.join(TOOLS, "romwbw-plm80"))


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


# What the console of a rebuild of HELLO.PLM showed after `$include
# (common.lit)` was added and COMMON.LIT did not exist - which romwbw-plm80
# reported as built, because the HELLO.COM the good build had left in -o was
# still there.
HELLO_FAILED = (
    "A>B:ISX\n\n\nISIS-II INTERFACE VERS 1.4\n\n"
    "0>:F1:PLM80 HELLO.PLM DEBUG PAGEWIDTH(80)\x00\n\n"
    "ISIS-II PL/M-80 COMPILER V3.1\n\n\n"
    "PL/M-80 I/O ERROR --\n  FILE: SOURCE\n  NAME: COMMON.LIT     \n"
    "  ERROR: 13--NO SUCH FILE\nCOMPILATION TERMINATED\n\n\n"
    "0>:F1:LINK HELLO.OBJ,:F1:X0100,:F1:PLM80.LIB TO HELLO.MOD\x00\n"
    "ISIS-II OBJECT LINKER V3.0\n :F0:HELLO.OBJ, NO SUCH FILE\n\n"
    "0>:F1:LOCATE HELLO.MOD CODE(0100H) STACKSIZE(100) MAP PRINT(HELLO.TRA)\x00\n"
    "ISIS-II OBJECT LOCATER V3.0\n :F0:HELLO.MOD, NO SUCH FILE\n\n"
    "0>:F1:CPM\x00\n\n\nRetroBrew SBC [SBC_simh_std] Boot Loader\n"
    "AutoBoot in 0 Seconds (<esc> aborts, <enter> now)...\n\n"
    "A>B:OBJCPM HELLO\n\nNO OBJECT FILE\nA>TYPE ROMWBW.END\n")

# A good build's console, with everything in it that looks like trouble and
# is not: the boot loader's "aborts", zero errors, ASM80's NO ERRORS, and a
# source whose name is a failure word, echoed after the ISX prompt.
GOOD_CONSOLE = (
    "AutoBoot in 0 Seconds (<esc> aborts, <enter> now)...\n"
    "0>:F1:PLM80 INVALID.PLM DEBUG PAGEWIDTH(80)\x00\n"
    "ISIS-II PL/M-80 COMPILER V3.1\n"
    "PL/M-80 COMPILATION COMPLETE.      0 PROGRAM ERROR(S)\n"
    "0>:F1:ASM80 LDRLWR.ASM DEBUG\x00\n"
    "ISIS-II 8080/8085 MACRO ASSEMBLER, V2.0\nASSEMBLY COMPLETE,  NO ERRORS\n"
    "A>PIP DIR.HEX=DIR1.HEX,DIR2.HEX\n\nA>B:GENMOD DIR.HEX DIR.PRL\n\n"
    "REL MOD END  050A\nREL MOD SIZE 05FF\nMODULE CONSTRUCTED\n")


def test_plm80_console():
    check(rp.check_console(GOOD_CONSOLE) == [],
          "romwbw-plm80: a good build's console reports nothing")
    p = rp.check_console(HELLO_FAILED)
    check(any("I/O ERROR" in x and "COMMON.LIT" in x and "NO SUCH FILE" in x
              and "COMPILATION TERMINATED" in x for x in p),
          "romwbw-plm80: PL/M-80's I/O error is a failure, reported whole - "
          "which file, and why")
    check(":F0:HELLO.OBJ, NO SUCH FILE" in p and "NO OBJECT FILE" in p,
          "romwbw-plm80: so are LINK's NO SUCH FILE and OBJCPM's NO OBJECT FILE")
    for text, what in (
            ("PL/M-80 FATAL ERROR --\n  DYNAMIC STORAGE OVERFLOW\n"
             "COMPILATION TERMINATED\n", "PL/M-80's fatal error"),
            ("A>PIP DIR.HEX=DIR1.HEX,DIR2.HEX\n\nNO FILE: DIR1.HEX\n",
             "PIP's NO FILE"),
            ("A>B:GENMOD DIR.HEX DIR.PRL\nBAD INPUT RECORD\n", "GENMOD's"),
            ("ISIS-II OBJECT LINKER V3.0\nUNRESOLVED EXTERNAL NAMES:\n"
             "    MON1\n", "LINK's unresolved names")):
        check(rp.check_console(text) != [], "romwbw-plm80: %s is a failure"
              % what)


class FakeBatch(object):
    """What build() asks of a Batch, with the run's console and disk made up.

    `files` is A: after the run; `before`, if given, is A: before it, which
    clear_outputs() looks at and edits."""

    def __init__(self, workdir, console, files, before=None):
        self.workdir, self.console, self.files = workdir, console, files
        self.a = dict(before) if before is not None else dict(files)
        self.ran = False

    def run(self, commands):
        self.ran = True
        r = rb.Result()
        r.console, r.completed, r.seconds = self.console, True, 0.1
        return r

    def _disk(self):
        return self.files if self.ran else self.a

    def names(self, drive="A"):
        return sorted(self._disk())

    def read(self, name, drive="A"):
        return self._disk()[name]

    def remove(self, name, drive="A"):
        return self.a.pop(name, None) is not None


def test_plm80_outputs():
    args = rp.build_parser().parse_args(["com", "HELLO.PLM"])
    cmds, outs = rp.build_commands("com", ["HELLO.PLM"], "HELLO", args)
    names = ["HELLO.COM", "HELLO.SYM", "HELLO.LIN", "HELLO.TRA", "HELLO.LST",
             "HELLO.OBJ"]
    check([h for _, h in outs] == names,
          "romwbw-plm80: com owns NAME.COM, .SYM, .LIN, .TRA and each "
          "source's .LST and .OBJ")

    def run(console, files):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "out")
            os.makedirs(out)
            for n in names:
                with open(os.path.join(out, n), "wb") as f:
                    f.write(b"from an earlier build")
            args.out, args.quiet, args.work = out, True, None
            with contextlib.redirect_stderr(io.StringIO()) as err:
                status = rp.build(FakeBatch(d, console, files), cmds, outs,
                                  args)
            left = {n: open(os.path.join(out, n), "rb").read()
                    for n in sorted(os.listdir(out))}
            return status, left, err.getvalue()

    # The rebuild that failed at its first step, with the last build's
    # output in -o: exit 0 and "built" is what this used to say.
    status, left, err = run(HELLO_FAILED + "ROMWBW-BATCH-END", {})
    check(status == 1 and "no HELLO.COM was produced" in err,
          "romwbw-plm80: a rebuild that made nothing fails, whatever -o holds")
    check(left == {}, "romwbw-plm80: ...and takes away the earlier build's "
                      "files, so none of them passes for this one's")

    made = {n: b"this build" for n in names}
    status, left, err = run(
        "PL/M-80 COMPILATION COMPLETE.      2 PROGRAM ERROR(S)\n", made)
    check(status == 1 and sorted(left) == ["HELLO.LST", "HELLO.TRA"] and
          left["HELLO.LST"] == b"this build",
          "romwbw-plm80: a compile with errors writes its listings, and not "
          "the .COM or the objects made from it")

    status, left, err = run(GOOD_CONSOLE, made)
    check(status == 0 and left == made,
          "romwbw-plm80: a good build writes all it made")

    # --name is the CP/M name; the ISIS steps use as much of it as ISIS takes.
    cmds8, outs8 = rp.build_commands("com", ["HELLO.PLM"], "HELLOWLD", args)
    check("B:OBJCPM HELLOW" in cmds8 and outs8[0] == ("HELLOW.COM",
                                                      "HELLOWLD.COM"),
          "romwbw-plm80: com --name HELLOWLD builds HELLOW and writes "
          "HELLOWLD.COM")
    pargs = rp.build_parser().parse_args(["prl", "CNS.PLM"])
    cmdsp, outsp = rp.build_commands("prl", ["CNS.PLM"],
                                     rp.output_name("console"), pargs)
    check(":F1:LOCATE CONSO1.MOD CODE(0100H) STACKSIZE(100)" in cmdsp and
          "B:GENMOD CONSO.HEX CONSOLE.PRL" in cmdsp and
          outsp[0] == ("CONSOLE.PRL", "CONSOLE.PRL"),
          "romwbw-plm80: prl --name CONSOLE, as DRI built it from CNS")
    for bad in ("DSKRESET1", "CONSOLE.PRL", "A-B"):
        try:
            rp.output_name(bad)
            ok = False
        except rp.rb.UsageError:
            ok = True
        check(ok, "romwbw-plm80: --name %s is refused" % bad)


# A: as hd1k_cpm22 has it, in part: RomWBW's own ED.COM, ASSIGN.COM and more.
SYSTEM_A = {"ED.COM": b"RomWBW's ED", "ASSIGN.COM": b"RomWBW's ASSIGN",
            "PIP.COM": b"RomWBW's PIP", "STAT.COM": b"RomWBW's STAT",
            "HELLO.PLM": b"the source"}

LOCATE_FAILED = (
    "0>:F1:LOCATE ED.MOD CODE(0100H) STACKSIZE(XYZ) MAP PRINT(ED.TRA)\x00\n"
    "ISIS-II OBJECT LOCATER V3.0\n\n"
    "A>B:OBJCPM ED\n\nNO OBJECT FILE\nA>TYPE ROMWBW.END\n")


def test_plm80_produced():
    """An output named like a file of the system disk's own is not taken for
    built: `romwbw-plm80 com HELLO.PLM --name ED --stack XYZ` failed in
    LOCATE and reported ED.COM produced, because A: - a copy of hd1k_cpm22 -
    had RomWBW's ED.COM."""
    def run(name, console, made, route="com", before=SYSTEM_A):
        args = rp.build_parser().parse_args([route, "HELLO.PLM"])
        cmds, outs = rp.build_commands(route, ["HELLO.PLM"], name, args)
        b = FakeBatch(None, console, None, before)
        kept = rp.clear_outputs(b, outs)
        after = dict(b.a)
        after.update(made)
        b.files = after
        with tempfile.TemporaryDirectory() as d:
            b.workdir = d
            args.out, args.quiet, args.work = os.path.join(d, "out"), True, None
            with contextlib.redirect_stderr(io.StringIO()) as err:
                status = rp.build(b, cmds, outs, args, kept)
            left = {n: open(os.path.join(args.out, n), "rb").read()
                    for n in sorted(os.listdir(args.out))}
        return status, left, err.getvalue(), b, kept

    status, left, err, b, kept = run("ED", LOCATE_FAILED, {})
    check("ED.COM" not in b.a and "STAT.COM" in b.a and kept == {},
          "romwbw-plm80: RomWBW's ED.COM is erased from A: before a build "
          "that writes ED.COM, and nothing else is")
    check(status == 1 and "no ED.COM was produced" in err and
          "ED.COM" not in left,
          "romwbw-plm80: --name ED whose LOCATE fails is not built, and "
          "RomWBW's ED.COM does not reach -o")
    status, left, err, b, kept = run("ED", GOOD_CONSOLE,
                                     {"ED.COM": b"this build"})
    check(status == 0 and left.get("ED.COM") == b"this build",
          "romwbw-plm80: --name ED that builds writes this build's ED.COM")

    # ASSIGN.COM swaps B: in, so it stays on A:, and must change to count.
    status, left, err, b, kept = run("ASSIGN", LOCATE_FAILED, {})
    check(kept == {"ASSIGN.COM": b"RomWBW's ASSIGN"} and
          "ASSIGN.COM" in b.a,
          "romwbw-plm80: ASSIGN.COM, which the batch runs, is kept on A:")
    check(status == 1 and "no ASSIGN.COM was produced: A: already had" in err
          and "ASSIGN.COM" not in left,
          "romwbw-plm80: ...and a build that leaves it unchanged produced "
          "nothing")
    status, left, err, b, kept = run("ASSIGN", GOOD_CONSOLE,
                                     {"ASSIGN.COM": b"this build"})
    check(status == 0 and left.get("ASSIGN.COM") == b"this build",
          "romwbw-plm80: ...and one that replaces it did")
    status, left, err, b, kept = run("PIP", LOCATE_FAILED, {})
    check("PIP.COM" not in b.a and status == 1 and "PIP.COM" not in left,
          "romwbw-plm80: PIP.COM is erased for com --name PIP, which does not "
          "run it, and not passed off as built")

    # --include cannot name a file the build writes: it would be erased.
    with tempfile.TemporaryDirectory() as d:
        tools = os.path.join(d, "tools")
        os.makedirs(tools)
        for n in rp.ISIS_TOOLS:
            open(os.path.join(tools, n), "wb").close()
        open(os.path.join(d, "HELLO.PLM"), "w").close()
        open(os.path.join(d, "HELLO.LST"), "w").close()
        with contextlib.redirect_stderr(io.StringIO()) as err:
            status = rp.main(["com", os.path.join(d, "HELLO.PLM"), "--tools",
                              tools, "--include", os.path.join(d, "HELLO.LST"),
                              "--offline"])
        check(status == 64 and "HELLO.LST is a name this build writes" in
              err.getvalue(),
              "romwbw-plm80: an --include named like one of the build's "
              "outputs is refused")


def test_installed_layout(cd):
    """The tools as the .deb and .rpm install them: /usr/bin/romwbw-batch and
    /usr/bin/romwbw-plm80, and cpmemu's cpm_disk.py as
    /usr/share/romwbw_emu/cpm_disk.py, with no cpmemu checkout beside them,
    no $CPM_DISK and no cpm_disk on PATH.  Up to 1.48 the packages carried
    neither tool, and a tool copied to /usr/bin found no cpm_disk.py."""
    saved = {k: os.environ.get(k) for k in ("CPM_DISK", "PATH")}
    saved_dwb = sys.dont_write_bytecode
    with tempfile.TemporaryDirectory() as d:
        usr = os.path.join(d, "usr")
        bindir = os.path.join(usr, "bin")
        share = os.path.join(usr, "share", "romwbw_emu")
        os.makedirs(bindir)
        os.makedirs(share)
        for tool in ("romwbw-batch", "romwbw-plm80"):
            shutil.copy2(os.path.join(TOOLS, tool), bindir)
        packaged = os.path.join(share, "cpm_disk.py")
        shutil.copy2(cd.__file__, packaged)
        empty = os.path.join(d, "empty")
        os.makedirs(empty)
        os.environ.pop("CPM_DISK", None)
        os.environ["PATH"] = empty
        try:
            # Run as a user runs them.  romwbw-plm80 loads romwbw-batch, and
            # romwbw-batch cpm_disk.py, as modules - which wrote a __pycache__
            # beside each, in /usr/bin and /usr/share/romwbw_emu, and put one
            # in the package when the release workflow ran them staged.
            env = dict(os.environ)
            env.pop("PYTHONDONTWRITEBYTECODE", None)
            p = subprocess.run([sys.executable,
                                os.path.join(bindir, "romwbw-plm80"), "--help"],
                               env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
            q = subprocess.run([sys.executable,
                                os.path.join(bindir, "romwbw-batch"), "-c",
                                "DIR", "--rom", os.path.join(d, "no.rom"),
                                "--emu", os.path.join(d, "no_emu")],
                               env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT,
                               universal_newlines=True)
            caches = [os.path.join(r, n) for r, dirs, files in os.walk(usr)
                      for n in dirs + files
                      if n == "__pycache__" or n.endswith(".pyc")]
            check(p.returncode == 0 and q.returncode == 2 and
                  "no emulator at" in q.stdout and not caches,
                  "installed: the tools run, and leave no bytecode cache in "
                  "/usr (%s)" % (", ".join(caches) or "none"))
            sys.dont_write_bytecode = True
            inst = load("romwbw_batch_installed",
                        os.path.join(bindir, "romwbw-batch"))
            found = inst.find_cpm_disk()
            check(os.path.realpath(found.__file__) ==
                  os.path.realpath(packaged),
                  "installed: romwbw-batch finds the package's cpm_disk.py "
                  "in /usr/share/romwbw_emu")
            plm = load("romwbw_plm80_installed",
                       os.path.join(bindir, "romwbw-plm80"))
            check(os.path.realpath(plm.rb.__file__) ==
                  os.path.realpath(os.path.join(bindir, "romwbw-batch")),
                  "installed: romwbw-plm80 loads the romwbw-batch beside it")
            os.remove(packaged)
            try:
                inst.find_cpm_disk()
                msg = ""
            except inst.BatchError as e:
                msg = str(e)
            check("/usr/share/romwbw_emu/cpm_disk.py" in msg and
                  "Looked for:" in msg and packaged in msg,
                  "installed without it: the error says where the package "
                  "puts it, and where it looked")
            with open(packaged, "w") as f:
                f.write("def get_disk_object(d): pass\n"
                        "def create_hd1k_disk(): pass\n"
                        "def filename_to_name83(n): pass\n"
                        "def entry_name83(e): pass\n"
                        "class Hd1kDisk(object):\n"
                        "    def add_file(self, n, d): pass\n"
                        "    def extract_file(self, n): pass\n"
                        "    def list_files(self): pass\n"
                        "    def delete_file(self, n, user=0): pass\n")
            try:
                inst.find_cpm_disk()
                msg = ""
            except inst.BatchError as e:
                msg = str(e)
            check("too old" in msg and "delete_file(exact=)" in msg and
                  "4.10.0" in msg,
                  "a cpm_disk.py older than 4.10.0 is refused by name, not "
                  "met half way through a batch")
        finally:
            sys.dont_write_bytecode = saved_dwb
            for k, v in saved.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v


# --- batches on the emulator ----------------------------------------------------

def test_arguments_first():
    """Both tools refuse a bad argument before they look for anything.

    `romwbw-plm80 --max-instructions -5` was refused only once romwbw-get had
    resolved the ROM and the disk, some four seconds in, and `romwbw-batch -a
    NOPE.COM` got as far and then exited 2 with Python's `[Errno 2] No such
    file or directory: 'NOPE.COM'`.  A ROM that cannot be resolved shows
    which comes first: checked first, the argument is what is reported."""
    with tempfile.TemporaryDirectory() as d:
        bad_rom = ["--rom", os.path.join(d, "no.rom"), "--offline"]
        r = run_tool([os.path.join(TOOLS, "romwbw-batch"), "-a", "NOPE.COM",
                      "-c", "DIR"] + bad_rom, d, timeout=60)
        check(r.returncode == 64 and
              "-a NOPE.COM: no such file or directory" in r.stdout and
              "Errno" not in r.stdout,
              "romwbw-batch -a NOPE.COM: refused first, plainly, as usage "
              "(exit %s)" % r.returncode)
        r = run_tool([os.path.join(TOOLS, "romwbw-batch"), "-t", "B:NOPE.TXT",
                      "-c", "DIR"] + bad_rom, d, timeout=60)
        check(r.returncode == 64 and "-t NOPE.TXT: no such file" in r.stdout,
              "romwbw-batch -t B:NOPE.TXT: the same")
        r = run_tool([os.path.join(TOOLS, "romwbw-batch"), "-c", "DIR",
                      "--max-instructions", "-5"] + bad_rom, d, timeout=60)
        check(r.returncode == 64 and "not a count of instructions" in r.stdout,
              "romwbw-batch --max-instructions -5: refused first (exit %s)"
              % r.returncode)
        r = run_tool([os.path.join(TOOLS, "romwbw-plm80"), "com", "X.PLM",
                      "--max-instructions", "-5"] + bad_rom, d, timeout=60)
        check(r.returncode == 64 and "not a count of instructions" in r.stdout,
              "romwbw-plm80 --max-instructions -5: refused first (exit %s)"
              % r.returncode)


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

        # -g reads A: as the run left it, and A: starts as a copy of the
        # system disk: after a rebuild of STAT.COM that failed, -g STAT.COM
        # brought back RomWBW's own, exit 0, and said nothing.
        stock = open(rb.resolve_asset(rb.DEFAULT_SYSTEM_DISK, offline=True),
                     "rb").read()
        r = batch(["-c", "DIR STAT.COM", "-g", "STAT.COM", "-o", "stock"], d)
        got = os.path.join(d, "stock", "STAT.COM")
        check(r.returncode == 0 and os.path.exists(got) and
              "STAT.COM is the system disk's own, byte for byte" in r.stdout,
              "-g of a system disk file the batch left alone warns that it "
              "is RomWBW's own")
        check(os.path.exists(got) and open(got, "rb").read()[:128] in stock,
              "...and the file extracted is indeed the system disk's")
        r = batch(["-t", "hi.txt", "-c", "PIP STAT.COM=HI.TXT", "-g",
                   "STAT.COM", "-o", "made"], d)
        got = os.path.join(d, "made", "STAT.COM")
        check(r.returncode == 0 and "system disk's own" not in r.stdout and
              os.path.exists(got) and
              open(got, "rb").read().startswith(b"hello\r\n"),
              "-g of a system disk file the batch rewrote says nothing")
        r = batch(["-c", "DIR", "-g", "PIP.COM", "-g", "STAT.COM", "-o",
                   "two"], d)
        check("2 of the files extracted are the system disk's own, byte for "
              "byte - the batch did not change them: PIP.COM, STAT.COM"
              in r.stdout, "...and several are named in one warning")

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

        # Piped stdin, without the batch tool: the boot loader used to read
        # the script's first line during its autoboot countdown and drop it,
        # and flush everything at its prompt.  The CLI now holds piped input
        # until the guest first asks for a key.
        emu = rb.find_emulator()
        rom = rb.resolve_asset("@rom", offline=True)
        img = os.path.join(d, "pipe.img")
        shutil.copyfile(rb.resolve_asset(rb.DEFAULT_SYSTEM_DISK, offline=True),
                        img)
        os.chmod(img, 0o644)
        for boot, script, want in (
                ("2", b"STAT DSK:\rSTAT\r", ["A>STAT DSK:", "Drive Characteristics",
                                             "A>STAT\n"]),
                ("H", b"2\rSTAT\r", ["Boot [H=Help]: 2", "A>STAT\n"])):
            p = subprocess.run([emu, "--romwbw=" + rom, "--disk0=" + img,
                                "--boot=" + boot, "--no-config"], input=script,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                               timeout=60)
            out = p.stdout.decode("latin-1").replace("\r", "")
            check(all(w in out for w in want),
                  "piped stdin with --boot=%s: the script's first line reaches "
                  "%s" % (boot, "the CCP" if boot == "2" else "the boot menu"))

        test_escape_stops_countdown(emu, rom, img)
        test_zpm3(emu, rom, d)
        test_os_starts(emu, rom, d)
        test_boot_image_polls(emu, rom, d)
        test_instruction_limit(emu, rom, d)
        test_escape_at_prompt(emu, rom)
        test_countdown_real_time(emu, rom)

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


def test_escape_stops_countdown(emu, rom, img):
    """A script led with Esc stops the autoboot countdown, as it did before
    the CLI held piped input: `(printf '\\033'; sleep 1.5; printf 'D\\r') |
    romwbw_emu --boot=2` stopped at the loader's prompt and listed the
    devices on 0448175, and booted CP/M on e5d61f0, whose CCP got `^[D`.
    Now a held Esc shows to the countdown and the rest stays held, so the
    prompt's flush finds nothing and the pause is not needed either.

    The Esc is in stdin before the emulator starts: the countdown looks once,
    early, and a pipe written after the start can miss it - as 0448175's
    could."""
    argv = [emu, "--romwbw=" + rom, "--disk0=" + img, "--boot=2",
            "--no-config"]

    def listed(out):
        return ("Boot [H=Help]: D" in out and "Capacity/Mode" in out and
                "CP/M-80" not in out)

    r, w = os.pipe()
    os.write(w, b"\x1b")
    p = subprocess.Popen(argv, stdin=r, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL)
    os.close(r)
    time.sleep(1.5)
    os.write(w, b"D\r")
    os.close(w)
    try:
        out = p.communicate(timeout=60)[0].decode("latin-1").replace("\r", "")
    except subprocess.TimeoutExpired:
        p.kill()
        out = p.communicate()[0].decode("latin-1")
    check(listed(out), "piped Esc, a pause, then D: the countdown stops and "
                       "the loader lists the devices")
    with tempfile.TemporaryFile() as f:
        f.write(b"\x1bD\r")
        f.seek(0)
        p = subprocess.run(argv, stdin=f, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, timeout=60)
    out = p.stdout.decode("latin-1").replace("\r", "")
    check(listed(out), "Esc and D in one go: the countdown stops, and the D "
                       "is not flushed at the prompt")


def test_zpm3(emu, rom, d):
    """ZPM3, slice 4 of hd1k_combo: its prompt reads the RTC between polls,
    3605 T-states apart, so the tight-poll rule alone never called it idle."""
    try:
        combo = rb.resolve_asset("hd1k_combo", offline=True)
    except rb.BatchError:
        skip("ZPM3 at end of input: hd1k_combo is not cached "
             "(tools/romwbw-get fetch hd1k_combo)")
        return
    img = os.path.join(d, "combo.img")
    shutil.copyfile(combo, img)
    os.chmod(img, 0o644)
    argv = [emu, "--romwbw=" + rom, "--disk0=" + img, "--boot=2.4",
            "--no-config"]
    try:
        subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=60)
        ended = True
    except subprocess.TimeoutExpired:
        ended = False
    check(ended, "ZPM3: with stdin at end of file the run ends at its prompt")
    try:
        p = subprocess.run(argv, input=b"DIR\r", stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, timeout=60)
        out = p.stdout.decode("latin-1")
    except subprocess.TimeoutExpired:
        out = ""
    check("Files Found" in out,
          "ZPM3: piped input reaches its prompt, and DIR runs")


def poll_program(org):
    """A program that prints a dot, polls the console and spins a while,
    60,000 times, straight to HBIOS (CALL 0FFF0H), then says what it found:
    "GOT k" or "STARVED".  Then it reads the console for ever, so at end of
    input the run ends.  60,000 turns is about four times as long as the hold
    lasts after the boot loader hands over."""
    code = bytearray()
    fix = {}

    def at():
        return org + len(code)

    def op(*b):
        code.extend(b)

    def ref(label):                      # a 16-bit address, patched below
        fix.setdefault(label, []).append(len(code))
        code.extend(b"\x00\x00")

    def hbios(func):                     # LD BC,func<<8|80H (the console)
        op(0x01, 0x80, func, 0xCD, 0xF0, 0xFF)

    labels = {}
    op(0x31, 0x00, (org >> 8) + 0x10)    # LD SP,org+1000H
    op(0x21, 0x60, 0xEA, 0x22); ref("count")      # LD HL,60000; LD (count),HL
    labels["loop"] = at()
    op(0x1E, ord(".")); hbios(0x01)                # CIOOUT '.'
    hbios(0x02)                                    # CIOIST
    op(0xB7, 0xC2); ref("got")                     # OR A; JP NZ,got
    for _ in range(6):
        op(0x06, 0x00, 0x10, 0xFE)                 # LD B,0; DJNZ $
    op(0x2A); ref("count")                         # LD HL,(count)
    op(0x2B, 0x22); ref("count")                   # DEC HL; LD (count),HL
    op(0x7C, 0xB5, 0xC2); ref("loop")              # LD A,H; OR L; JP NZ,loop
    op(0x21); ref("starved")                       # LD HL,starved
    op(0xC3); ref("print")
    labels["got"] = at()
    hbios(0x00)                                    # CIOIN
    op(0x7B, 0x32); ref("key")                     # LD A,E; LD (key),A
    op(0x21); ref("gotmsg")
    labels["print"] = at()
    op(0x7E, 0xB7, 0xCA); ref("done")              # LD A,(HL); OR A; JP Z,done
    op(0xE5, 0x5F); hbios(0x01); op(0xE1, 0x23)    # PUSH HL; LD E,A; CIOOUT...
    op(0xC3); ref("print")
    labels["done"] = at()
    hbios(0x00)                                    # CIOIN, for ever
    op(0xC3); ref("done")
    labels["count"] = at()
    op(0, 0)
    labels["starved"] = at()
    code.extend(b"\r\nSTARVED\r\n\x00")
    labels["gotmsg"] = at()
    code.extend(b"\r\nGOT ")
    labels["key"] = at()
    code.extend(b"?\r\n\x00")
    for label, offs in fix.items():
        for o in offs:
            code[o:o + 2] = labels[label].to_bytes(2, "little")
    return bytes(code)


def test_boot_image_polls(emu, rom, d):
    """A program that runs before anything reads a key and prints between
    polls - a game loop, a display started at boot - gets piped input.

    The CLI holds piped input back from the boot loader, which read and
    dropped a script's first line.  It let the input go only when the guest
    read a key or sat in a wait loop, and a program like this does neither:
    each line it prints resets the idle count.  So it never saw the input,
    where on 0448175 it did.  Now the hold lets go 120M T-states after the
    loader hands over to what it booted - about a second here.  The program
    is booted as the disk's OS, the way romldr boots CP/M."""
    org = 0xD000
    prog = poll_program(org)
    img = os.path.join(d, "poll.img")
    shutil.copyfile(rb.resolve_asset(rb.DEFAULT_SYSTEM_DISK, offline=True), img)
    os.chmod(img, 0o644)
    cd = rb.find_cpm_disk()
    base = getattr(rb.Drive(cd, img).disk, "base", 0)
    with open(img, "r+b") as f:
        # The boot record, as rb.SystemImage reads it: label, then load
        # address, end and entry; the image from the next sector.
        rec = base + rb.SystemImage.RECORD
        f.seek(rec + 0x1E7)
        f.write(b"POLLTEST$")
        f.seek(rec + 0x1FA)
        f.write(org.to_bytes(2, "little") + (org + 0x200).to_bytes(2, "little")
                + org.to_bytes(2, "little"))
        f.seek(base + rb.SystemImage.IMAGE)
        f.write(prog + bytes(0x200 - len(prog)))
    try:
        p = subprocess.run([emu, "--romwbw=" + rom, "--disk0=" + img,
                            "--boot=2", "--no-config"], input=b"K",
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                           timeout=60)
        out = p.stdout.decode("latin-1")
    except subprocess.TimeoutExpired:
        out = ""
    turns = out.count(".", out.find("POLLTEST"))
    check("POLLTEST" in out and "GOT K" in out,
          "piped input reaches a program the loader boots that prints between "
          "polls and never waits (%s after %d turns)"
          % ("GOT K" if "GOT K" in out else "STARVED" if "STARVED" in out
             else "no result", turns))


def test_os_starts(emu, rom, d):
    """CP/M 3 and NZ-COM, slices 3 and 2 of hd1k_combo, poll the console as
    they start and take what they find: released as soon as the boot loader
    handed over, a piped `DIR` reached CP/M 3's prompt as `IR`, and NZ-COM's
    too.  The hold keeps it until their prompts wait for a key."""
    try:
        combo = rb.resolve_asset("hd1k_combo", offline=True)
    except rb.BatchError:
        skip("CP/M 3 and NZ-COM starting: hd1k_combo is not cached "
             "(tools/romwbw-get fetch hd1k_combo)")
        return
    img = os.path.join(d, "combo.img")
    for slice_, name in (("3", "CP/M 3"), ("2", "NZ-COM")):
        shutil.copyfile(combo, img)
        os.chmod(img, 0o644)
        try:
            p = subprocess.run([emu, "--romwbw=" + rom, "--disk0=" + img,
                                "--boot=2." + slice_, "--no-config"],
                               input=b"DIR\r", stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, timeout=60)
            out = p.stdout.decode("latin-1").replace("\r", "")
        except subprocess.TimeoutExpired:
            out = ""
        typed = re.findall(r"^\S*>(\S*)", out, re.M)
        check("DIR" in typed,
              "piped stdin: %s's prompt gets the whole first line (typed at "
              "its prompts: %s)" % (name, typed))


def test_instruction_limit(emu, rom, d):
    """The emulator stops a run at an instruction limit.  It was ten billion,
    a constant with no option, and a run that reached it exited 0, which a
    script could not tell from the guest finishing.  Now it is
    --max-instructions, and a run stopped there exits 124.

    The default is for a run nobody watches: with stdin a terminal there is
    none, where seven minutes of work at a prompt used to end with 124.  A
    run with a limit names it at the start, which is how the terminal case
    is checked without running ten billion instructions."""
    def run(*opts):
        p = subprocess.run([emu, "--romwbw=" + rom, "--boot=H", "--no-config"]
                           + list(opts), stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                           timeout=60)
        return p.returncode, p.stderr.decode("latin-1")
    rc, err = run("--max-instructions=1000")
    check(rc == 124 and "instruction limit, 1000 instructions" in err,
          "a run stopped at --max-instructions exits 124 and says so "
          "(exit %s)" % rc)
    rc, err = run()
    check(rc == 0, "a run that ends at the boot prompt, at end of input, "
                   "exits 0")
    check("Instruction limit: 10000000000 " in err,
          "stdin not a terminal: the default limit, ten billion, is in force "
          "and said")
    rc, _ = run("--max-instructions=0")
    check(rc == 0, "--max-instructions=0 is no limit")
    rc, err = run("--max-instructions=1e9")
    check(rc == 1 and "Invalid --max-instructions" in err,
          "--max-instructions=1e9 is refused, not read as 1")
    r = batch(["-c", "DIR", "--max-instructions", "200000"], d)
    check(r.returncode == 1 and
          "instruction limit (200,000 instructions)" in r.stdout,
          "romwbw-batch: a batch the emulator stopped at its instruction "
          "limit fails, and says so")

    try:
        import pty
        import select
        import signal
    except ImportError:
        skip("the instruction limit on a terminal: no pty module here")
        return

    def on_tty(opts, until, secs=20):
        """Run on a pty until `until` is printed or the emulator exits:
        (what it printed, its exit status or None if still running)."""
        pid, fd = pty.fork()
        if pid == 0:
            try:
                os.execv(emu, [emu, "--romwbw=" + rom, "--boot=H",
                               "--no-config"] + opts)
            finally:
                os._exit(127)
        buf, status = bytearray(), None
        end = time.time() + secs
        while time.time() < end and not (until and until in buf):
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    buf.extend(os.read(fd, 65536))
                except OSError:
                    pass
            got, st = os.waitpid(pid, os.WNOHANG)
            if got:
                status = os.WEXITSTATUS(st) if os.WIFEXITED(st) else -1
                break
        if status is None:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        os.close(fd)
        return bytes(buf), status
    out, status = on_tty([], b"Boot [H=Help]:")
    check(b"Boot [H=Help]:" in out and b"Instruction limit" not in out,
          "stdin a terminal: no instruction limit unless one is given")
    out, status = on_tty(["--max-instructions=1000"], None)
    check(status == 124 and b"Instruction limit: 1000 " in out,
          "stdin a terminal: a limit given is kept, and a run stopped there "
          "exits 124 (exit %s)" % status)


def test_escape_at_prompt(emu, rom):
    """^E at a prompt that polls the console, on a terminal: the boot loader's.

    While the console is idle the CLI sleeps 10 ms per poll, and it looked for
    its escape key only every 1024th instruction.  The boot loader's prompt
    polls every few dozen instructions, so that was 20 to 40 polls: ^E took
    0.1 to 0.4 s to reach sim>, where it had taken 0.02, and as long at CP/M
    3's and Z3PLUS's prompts.  It now looks after every idle poll, before
    sleeping - a median of 0.003-0.013 s here, against 0.106-0.138 before."""
    try:
        import pty
        import select
        import signal
        import time
    except ImportError:
        skip("^E at the boot prompt: no pty module here")
        return
    pid, fd = pty.fork()
    if pid == 0:
        try:
            os.execv(emu, [emu, "--romwbw=" + rom, "--boot=H", "--no-config"])
        finally:
            os._exit(127)
    buf = bytearray()

    def read_until(want, secs, start=0):
        end = time.time() + secs
        while time.time() < end:
            if want is not None and want in buf[start:]:
                return True
            r, _, _ = select.select([fd], [], [], 0.002)
            if r:
                try:
                    buf.extend(os.read(fd, 65536))
                except OSError:
                    return False
        return want is not None and want in buf[start:]

    lat = []
    try:
        # --boot=H goes to the prompt, whatever NVRAM holds.
        if read_until(b"Boot [H=Help]:", 20):
            read_until(None, 0.5)                 # settle into the poll loop
            for _ in range(5):
                start = len(buf)
                t0 = time.time()
                os.write(fd, b"\x05")
                if not read_until(b"sim>", 5, start):
                    break
                lat.append(time.time() - t0)
                os.write(fd, b"c\r")              # continue
                read_until(None, 0.3)
    finally:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        os.close(fd)
    lat.sort()
    median = lat[len(lat) // 2] if len(lat) == 5 else None
    check(median is not None and median < 0.06,
          "^E at the boot prompt reaches sim> at once (median of 5: %s)"
          % ("%.3fs" % median if median is not None else "no sim>"))


def test_countdown_real_time(emu, rom):
    """A SYSCONF-set autoboot countdown, on a terminal, takes the seconds it
    says.  romldr times it with a delay loop calibrated to the 4 MHz the
    HBIOS reports, and from 1.48, when the idle rule stopped counting it as
    waiting, it ran at host speed: `S AB E,3` was over in 0.40 s.  (1.47
    slept 10 ms after every instruction of it, and a one-second countdown
    had not finished after 400 s.)  The CLI now paces the loader's polls to
    the time each turn means - `S AB E,2` should take two seconds, from
    `AutoBoot in 2` to `AutoBoot in 0`."""
    try:
        import pty
        import select
        import signal
        import time
    except ImportError:
        skip("the autoboot countdown's length: no pty module here")
        return
    cfg = tempfile.mkdtemp(prefix="romwbw-countdown-")
    pid, fd = pty.fork()
    if pid == 0:
        try:
            # SYSCONF changes NVRAM, which the CLI saves at exit: not to the
            # config directory of whoever runs the tests.
            os.environ["XDG_CONFIG_HOME"] = cfg
            os.execv(emu, [emu, "--romwbw=" + rom, "--boot=H", "--no-config"])
        finally:
            os._exit(127)
    buf = bytearray()
    pos = [0]

    def wait_for(want, secs):
        """Read until `want` appears after the last match; its time, or None."""
        end = time.time() + secs
        while time.time() < end:
            i = buf.find(want, pos[0])
            if i >= 0:
                pos[0] = i + len(want)
                return time.time()
            r, _, _ = select.select([fd], [], [], 0.002)
            if r:
                try:
                    buf.extend(os.read(fd, 65536))
                except OSError:
                    return None
        return None

    def type_line(text):
        for ch in text:
            os.write(fd, ch.encode())
            time.sleep(0.02)

    took = None
    try:
        if wait_for(b"Boot [H=Help]:", 20):
            time.sleep(0.3)                       # past the prompt's flush
            type_line("W\r")
            if wait_for(b"$", 10):
                type_line("S AB E,2\r")
                if wait_for(b"Timeout = 2)", 10) and wait_for(b"$", 10):
                    type_line("X\r")
                    if wait_for(b"Boot [H=Help]:", 10):
                        time.sleep(0.3)
                        type_line("R\r")
                        t0 = wait_for(b"AutoBoot in 2 ", 20)
                        t1 = wait_for(b"AutoBoot in 0 ", 20) if t0 else None
                        if t1:
                            took = t1 - t0
    finally:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        os.close(fd)
        shutil.rmtree(cfg, ignore_errors=True)
    # Not under 1.9 s, which is the bug.  The upper bound only says it is
    # paced and not stuck: a loaded host can stall the emulator for longer
    # than it can catch up, and one run in the full suite at load average 30
    # took 3.29 s, where alone it takes 1.98.
    check(took is not None and 1.9 <= took <= 5.0,
          "a SYSCONF-set `S AB E,2` countdown takes two seconds on a "
          "terminal (%s)" % ("%.2fs" % took if took is not None
                             else "no countdown seen"))


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
                test_isx_rebuild(tools_dir, src, d)
                test_isx_system_names(tools_dir, src, first)
            else:
                check(built is not None and built == first,
                      "ISX: both modes build the same DIFF1.COM")

    # --isx=compact with every tool on A: and nothing added to B:.  PL/M-80's
    # work files go to :F1:, and the compact BIOS had no B: to put them on.
    with tempfile.TemporaryDirectory() as d:
        r = batch(["--isx", "-a", tools_dir, "-t", src, "-c", "ISX",
                   "-c", "PLM80 DIFF1.PLM", "-c", "CPM", "-g", "DIFF1.OBJ",
                   "-o", "out"], d, timeout=600)
        check(r.returncode == 0 and "Select" not in r.stdout,
              "ISX (compact): PL/M-80 runs with nothing added to B:")


def test_isx_system_names(tools_dir, src, built):
    """DIFF1 built as ED.COM, a name hd1k_cpm22 has a file of its own under.
    With a stack size LOCATE cannot read, the build fails - and it used to
    report ED.COM produced, and write RomWBW's ED.COM to -o."""
    with tempfile.TemporaryDirectory() as d:
        r = run_tool([os.path.join(TOOLS, "romwbw-plm80"), "com", src,
                      "--name", "ED", "--stack", "XYZ", "--tools", tools_dir,
                      "--offline", "-o", "out"], d, timeout=600)
        check(r.returncode == 1 and "no ED.COM was produced" in r.stdout and
              not os.path.exists(os.path.join(d, "out", "ED.COM")),
              "ISX (compact): --name ED --stack XYZ fails in LOCATE, and "
              "RomWBW's ED.COM is not taken for its output")
        r = run_tool([os.path.join(TOOLS, "romwbw-plm80"), "com", src,
                      "--name", "ED", "--tools", tools_dir, "--offline",
                      "-o", "out"], d, timeout=600)
        com = os.path.join(d, "out", "ED.COM")
        got = open(com, "rb").read() if os.path.exists(com) else None
        check(r.returncode == 0 and got is not None and got == built,
              "ISX (compact): --name ED that builds writes DIFF1's code as "
              "ED.COM")


def test_isx_rebuild(tools_dir, src, d):
    """Rebuild DIFF1 into the same -o after breaking it: an $INCLUDE of a
    file that is not there.  This said "built" and exited 0, because
    out/DIFF1.COM from the good build was still there."""
    with open(src, encoding="latin-1") as f:
        text = f.read()
    with open(os.path.join(d, "DIFF1.PLM"), "w", encoding="latin-1") as f:
        f.write("$include (nosuch.lit)\n" + text)
    r = run_tool([os.path.join(TOOLS, "romwbw-plm80"), "com", "DIFF1.PLM",
                  "--tools", tools_dir, "--offline", "-o", "out"], d,
                 timeout=600)
    check(r.returncode == 1 and "I/O ERROR" in r.stdout and
          "NOSUCH.LIT" in r.stdout and "FAILED" in r.stdout,
          "ISX (compact): a rebuild that cannot open its $INCLUDE fails, and "
          "says why")
    check(not os.path.exists(os.path.join(d, "out", "DIFF1.COM")),
          "ISX (compact): ...and the good build's DIFF1.COM is not left in -o "
          "to pass for its output")


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
    test_plm80_console()
    test_plm80_outputs()
    test_plm80_produced()
    if cd:
        test_installed_layout(cd)
    else:
        skip("the tools as a package installs them: cpm_disk.py not found")
    test_arguments_first()

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
