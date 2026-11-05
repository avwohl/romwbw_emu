#!/usr/bin/env python3
"""gdb_stub_test.py - the GDB stub of romwbw_emu (docs/GDB.md).

A small client of the GDB remote serial protocol, so the test needs no GDB.
Each case starts romwbw_emu with --gdb=0 and --gdb-port-file, connects, and
checks one thing that a debugger depends on:

  * connect: the stop reply, the 26-byte register reply, the packet size, and
    that the program has not run an instruction (PC 0000, ROM bank 00);
  * step, register write, and a breakpoint at INNER of tests/gdb_prog.asm;
  * the bank view: "monitor bank XX" shows ROM bank 00 and RAM bank 8E in the
    low 32K and the same common bank above 8000H, against the ROM file and
    the .COM file;
  * the wait-program rule, with the bytes of the .COM and with its length and
    SHA-256, for a program that CP/M 2.2 loads from a disk image; a wrong
    program never stops it; and the stop is at 0100H in bank 8EH;
  * "monitor bt" at INNER shows INNER, OUTER and the main routine;
  * ^C (the 03H byte) stops a guest that waits for a key, k ends romwbw_emu,
    D lets the program run on, the W packet carries the exit status, and the
    instruction limit gives status 124;
  * the sim> console still works while the stub is attached (on a pty).

Needs src/romwbw_emu built with the stub (cpmemu's gdbstub.cc beside this
checkout), a cached ROM and hd1k_cpm22 (tools/romwbw-get fetch @rom
hd1k_cpm22), um80 and ul80, and cpm_disk.py.  Skips, with the reason, when the
binary, the ROM, the disk or a pty is missing.

Run: python3 tests/gdb_stub_test.py      (from anywhere)
     make -C src test
"""

import importlib.machinery
import importlib.util
import os
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EMU = os.path.join(ROOT, "src", "romwbw_emu")

failures = []


def check(ok, what):
    print(("PASS" if ok else "FAIL") + ": " + what)
    if not ok:
        failures.append(what)


class Rsp:
    """The client end of the remote serial protocol, in the small."""

    def __init__(self, port, host="127.0.0.1"):
        self.sock = socket.create_connection((host, port), timeout=30)
        self.buf = b""

    def send_raw(self, data):
        self.sock.sendall(data)

    def send_packet(self, payload):
        data = payload.encode("latin-1")
        self.sock.sendall(b"$" + data + b"#%02x" % (sum(data) & 0xFF))

    def recv(self, timeout=30):
        self.sock.settimeout(timeout)
        while True:
            i = self.buf.find(b"$")
            j = self.buf.find(b"#", i + 1) if i >= 0 else -1
            if i >= 0 and j >= 0 and len(self.buf) >= j + 3:
                body = self.buf[i + 1:j].decode("latin-1")
                self.buf = self.buf[j + 3:]
                self.sock.sendall(b"+")
                return body
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("the stub closed the connection")
            self.buf += chunk

    def cmd(self, payload, timeout=30):
        self.send_packet(payload)
        return self.recv(timeout)

    def monitor(self, text, timeout=30):
        reply = self.cmd("qRcmd," + text.encode().hex(), timeout)
        return bytes.fromhex(reply).decode("latin-1")

    def mem(self, addr, n):
        return bytes.fromhex(self.cmd("m%x,%x" % (addr, n)))

    def reg(self, index):
        return int.from_bytes(bytes.fromhex(self.cmd("p%x" % index)), "little")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Emu:
    """romwbw_emu with the stub, on pipes.  stdin carries `keys`."""

    def __init__(self, rom, disk, keys=b"GP\r", extra=(), close_stdin=True, host="127.0.0.1"):
        self.dir = tempfile.mkdtemp(prefix="gdbtest-")
        portfile = os.path.join(self.dir, "port")
        argv = [EMU, "--romwbw=" + rom, "--disk0=" + disk, "--boot=2", "--no-config",
                "--max-instructions=0", "--gdb=0", "--gdb-port-file=" + portfile]
        argv += list(extra)
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.out = b""
        self.err = b""
        self.threads = []
        for name, stream in (("out", self.proc.stdout), ("err", self.proc.stderr)):
            t = threading.Thread(target=self._drain, args=(name, stream), daemon=True)
            t.start()
            self.threads.append(t)
        if keys:
            self.proc.stdin.write(keys)
            self.proc.stdin.flush()
        if close_stdin:
            self.proc.stdin.close()
        deadline = time.time() + 30
        port = None
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("romwbw_emu ended before it listened: " +
                                   self.err.decode("latin-1")[-300:])
            try:
                text = open(portfile).read().strip()
                if text:
                    port = int(text)
                    break
            except (OSError, ValueError):
                pass
            time.sleep(0.02)
        if port is None:
            self.proc.kill()
            raise RuntimeError("romwbw_emu wrote no port in 30 s")
        self.port = port
        self.rsp = Rsp(port, host)

    def _drain(self, name, stream):
        while True:
            data = os.read(stream.fileno(), 65536)
            if not data:
                return
            if name == "out":
                self.out += data
            else:
                self.err += data

    def finish(self, timeout=30):
        """Wait for the process, and return its exit status."""
        try:
            status = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            status = self.proc.wait()
            status = None
        for t in self.threads:
            t.join(2)
        self.rsp.close()
        shutil.rmtree(self.dir, ignore_errors=True)
        return status

    def kill(self):
        try:
            self.rsp.send_raw(b"$k#6b")
        except OSError:
            pass
        return self.finish(10)

    def text(self):
        return self.out.decode("latin-1")


def load_batch_module():
    batch = importlib.machinery.SourceFileLoader(
        "romwbw_batch", os.path.join(ROOT, "tools", "romwbw-batch"))
    spec = importlib.util.spec_from_loader("romwbw_batch", batch)
    mod = importlib.util.module_from_spec(spec)
    batch.exec_module(mod)
    return mod


def build_program(work):
    """tests/gdb_prog.asm -> GP.COM in `work`; the bytes."""
    src = os.path.join(HERE, "gdb_prog.asm")
    rel = os.path.join(work, "gp.rel")
    com = os.path.join(work, "gp.com")
    for tool in ("um80", "ul80"):
        if shutil.which(tool) is None:
            raise RuntimeError("%s is not on PATH (pip install um80)" % tool)
    subprocess.run(["um80", "-o", rel, src], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["ul80", "-o", com, rel], check=True, stdout=subprocess.DEVNULL)
    return com, open(com, "rb").read()


def disk_with(rb, work, com):
    img = os.path.join(work, "disk.img")
    shutil.copyfile(rb.resolve_asset(rb.DEFAULT_SYSTEM_DISK, offline=True), img)
    os.chmod(img, 0o644)
    cpm_disk = rb.find_cpm_disk().__file__
    subprocess.run([sys.executable, cpm_disk, "add", "--slice", "0", img, com],
                   check=True, stdout=subprocess.DEVNULL)
    return img


# The offsets in tests/gdb_prog.asm, checked against the linked bytes.
A_CALL_OUTER_RET = 0x010B   # the return address of "call outer"
A_OUTER = 0x0116
A_INNER = 0x011A
A_CALL_INNER_RET = 0x0119   # the return address of "call inner"


def lan_address():
    """An IPv4 address of this host that is not loopback, or None.  A UDP
    connect sends nothing; it only makes the system choose an interface."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))
        addr = s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()
    return None if addr.startswith("127.") or addr == "0.0.0.0" else addr


def refused(host, port):
    try:
        socket.create_connection((host, port), timeout=5).close()
    except OSError:
        return True
    return False


def case_listen(rom, disk):
    """--gdb-listen=ADDR: the stub on a network address (cpmemu's option)."""
    p = subprocess.run([EMU, "--romwbw=" + rom, "--no-config", "--gdb-listen=0.0.0.0"],
                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=30)
    check(p.returncode == 1 and b"--gdb-listen needs --gdb" in p.stderr,
          "--gdb-listen without --gdb is an error (exit 1)")
    for bad in ("1.2.3", "300.1.1.1", "localhost"):
        p = subprocess.run([EMU, "--romwbw=" + rom, "--no-config", "--gdb", "--gdb-listen=" + bad],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=30)
        check(p.returncode == 1 and b"Invalid --gdb-listen address" in p.stderr,
              "--gdb-listen=%s is refused (exit 1)" % bad)
    lan = lan_address()
    e = Emu(rom, disk, keys=b"")
    if lan:
        check(refused(lan, e.port), "without --gdb-listen the stub refuses %s" % lan)
    check(e.rsp.cmd("?") == "T05", "without --gdb-listen a client on 127.0.0.1 gets T05")
    e.kill()
    check(b"WARNING" not in e.err, "without --gdb-listen there is no warning")
    if not lan:
        print("NOTE: no LAN address on this host; the LAN cases were not run")
        return
    e = Emu(rom, disk, keys=b"", extra=["--gdb-listen=0.0.0.0"], host=lan)
    check(e.rsp.cmd("?") == "T05" and e.rsp.reg(5) == 0,
          "--gdb-listen=0.0.0.0: a client on %s stops at 0000" % lan)
    e.kill()
    check(b"WARNING: listening on 0.0.0.0" in e.err, "--gdb-listen=0.0.0.0 prints the warning")
    e = Emu(rom, disk, keys=b"", extra=["--gdb-listen=" + lan], host=lan)
    check(refused("127.0.0.1", e.port), "--gdb-listen=%s: 127.0.0.1 is refused" % lan)
    check(e.rsp.cmd("?") == "T05", "--gdb-listen=%s: a client on %s gets T05" % (lan, lan))
    e.kill()


def case_connect(rom, disk):
    e = Emu(rom, disk, keys=b"")
    r = e.rsp
    check(r.cmd("?") == "T05", "the stop reply to ? is T05")
    sup = r.cmd("qSupported:swbreak+;vContSupported+")
    check("PacketSize=" in sup and "swbreak+" in sup, "qSupported names the packet size")
    g = r.cmd("g")
    check(len(g) == 52, "the g reply is 26 bytes of register (got %d hex digits)" % len(g))
    check(r.reg(5) == 0, "PC is 0000: the program has not run an instruction")
    st = r.monitor("status")
    check("CPU bank 00" in st, "the CPU is in ROM bank 00 before the first instruction")
    check(r.cmd("vCont?") == "vCont;c;C;s;S;r", "vCont? lists c, C, s, S and r")
    check(r.cmd("P3=3412") == "OK" and r.reg(3) == 0x1234, "P writes HL (low byte first) and p reads it back")
    r.cmd("P3=0000")
    r.send_packet("s")
    check(r.recv() == "T05", "s answers T05")
    check(r.reg(5) != 0, "a step moved PC off 0000")
    help_text = r.monitor("nosuchcommand")
    check("wait-program" in help_text, "an unknown monitor command lists the commands")
    check(r.monitor("wait-program sha256 5 zz").startswith("wait-program sha256:"),
          "a bad wait-program argument is refused")
    check(r.monitor("bank 7f").startswith("bank:"), "bank 7f is not a bank")
    e.kill()


def run_to_program(e, com_bytes, how):
    """Arm wait-program, continue, and return the monitor status text."""
    r = e.rsp
    if how == "bytes":
        arm = r.monitor("wait-program bytes " + com_bytes[:24].hex())
    else:
        import hashlib
        n = len(com_bytes.rstrip(b"\x00"))   # CP/M pads the file to 128 bytes
        arm = r.monitor("wait-program sha256 %d %s" %
                        (n, hashlib.sha256(com_bytes[:n]).hexdigest()))
    check("armed" in arm, "wait-program %s is armed (%r)" % (how, arm))
    r.send_packet("c")
    return r.recv(60)


def case_program(rom, disk, com_bytes, how):
    e = Emu(rom, disk)
    r = e.rsp
    stop = run_to_program(e, com_bytes, how)
    # No debugger set a breakpoint at 0100H, so the rule's stop has no
    # swbreak: GDB would continue past a swbreak it did not ask for.
    check(stop == "T05", "wait-program %s: the stop reply is a plain T05 (%s)" % (how, stop))
    check(r.reg(5) == 0x0100, "wait-program %s: PC is 0100H" % how)
    st = r.monitor("status")
    check("CPU bank 8E" in st, "wait-program %s: the CPU is in bank 8EH" % how)
    check("not armed" in st, "wait-program %s: the rule is one-shot" % how)
    check(r.mem(0x100, 24) == com_bytes[:24], "wait-program %s: the bytes at 0100H are the .COM's" % how)
    if how == "bytes":
        case_banks_and_bt(e, r, rom, com_bytes)
    e.rsp.send_packet("c")
    reply = e.rsp.recv(60)
    check(reply == "W00", "the program ends with W00 (got %s)" % reply)
    status = e.finish()
    check(status == 0, "romwbw_emu exits 0 (got %s)" % status)
    out = e.text()
    check("GDBTEST1" in out and "GDBDONE" in out,
          "the program printed both lines (stdout %r)" % out[-80:])


def case_banks_and_bt(e, r, rom, com_bytes):
    rom_bytes = open(rom, "rb").read()
    check(r.monitor("bank 00").startswith("memory view: bank 00"), "monitor bank 00")
    check(r.mem(0, 16) == rom_bytes[:16], "bank 00 below 8000H is the start of the ROM file")
    low_rom = r.mem(0x9000, 32)
    check(r.monitor("bank 8e").startswith("memory view: bank 8E"), "monitor bank 8e")
    check(r.mem(0x100, 24) == com_bytes[:24], "bank 8E at 0100H is the program")
    check(r.mem(0x9000, 32) == low_rom, "8000H and up is the same common bank in both views")
    check(r.mem(0x7FFF, 2) == r.mem(0x7FFF, 1) + r.mem(0x8000, 1),
          "a read across 8000H joins bank 8E and the common bank")
    # A write goes to the viewed bank: the CPU is in 8EH, so it is the program's.
    check(r.cmd("M300,1:a5") == "OK", "M writes through the view")
    r.monitor("bank 00")
    check(r.mem(0x300, 1) != b"\xa5", "the write did not reach ROM bank 00")
    r.monitor("bank cur")
    check(r.mem(0x300, 1) == b"\xa5", "bank cur shows the write in the CPU's bank")
    check(r.cmd("M300,1:00") == "OK", "restore the byte")

    # The shadow stack starts at the program: INNER, OUTER, the main routine,
    # and the CCP's return address below them.
    check("restarted" in r.monitor("bt-reset"), "bt-reset restarts the shadow stack")
    check(r.cmd("Z0,%x" % A_INNER) == "OK", "Z0 sets a breakpoint at INNER")
    r.send_packet("c")
    stop = r.recv(60)
    check(stop == "T05swbreak:;", "the breakpoint at INNER stops the program (%s)" % stop)
    check(r.reg(5) == A_INNER, "PC is INNER")
    bt = r.monitor("bt").splitlines()
    check(bt[0] == "#0  0x%04X" % A_INNER, "bt frame 0 is INNER (%r)" % bt[0])
    check(bt[1].startswith("#1  0x%04X" % A_CALL_INNER_RET), "bt frame 1 is OUTER (%r)" % bt[1])
    check(bt[2].startswith("#2  0x%04X" % A_CALL_OUTER_RET), "bt frame 2 is the main routine (%r)" % bt[2])
    check(len(bt) >= 4 and bt[3].startswith("#3"), "bt frame 3 is the CCP's")
    r.send_packet("s")
    r.recv()
    check(r.reg(5) == A_INNER + 2, "a step runs LD A,5 (PC 011C)")
    check((r.reg(0) >> 8) == 5, "and A is 5")
    check(r.cmd("z0,%x" % A_INNER) == "OK", "z0 clears the breakpoint")
    check("applies in bank 8E only" in r.monitor("break-bank") or "bank 8E" in r.monitor("break-bank"),
          "after the program is found, breakpoints below 8000H apply in bank 8E")


def case_wrong_program(rom, disk, com_bytes):
    e = Emu(rom, disk)
    r = e.rsp
    wrong = bytearray(com_bytes[:24])
    wrong[2] ^= 0xFF
    r.monitor("wait-program bytes " + bytes(wrong).hex())
    r.send_packet("c")
    reply = r.recv(60)
    check(reply == "W00", "a wait-program that does not match never stops: the run ends (%s)" % reply)
    status = e.finish()
    check(status == 0, "romwbw_emu exits 0 (got %s)" % status)
    check("GDBDONE" in e.text(), "the program ran to its end")

    e = Emu(rom, disk)
    r = e.rsp
    r.monitor("wait-program sha256 %d %s" % (len(com_bytes.rstrip(b"\x00")), "00" * 32))
    r.send_packet("c")
    reply = r.recv(60)
    check(reply == "W00", "a wrong SHA-256 does not stop it either (%s)" % reply)
    e.finish()


def case_interrupt(rom, disk):
    # No keys and stdin open: the guest ends at the CCP prompt and waits.
    e = Emu(rom, disk, keys=b"", close_stdin=False)
    r = e.rsp
    r.send_packet("c")
    time.sleep(3)
    t0 = time.time()
    r.send_raw(b"\x03")
    try:
        reply = r.recv(15)
    except Exception as ex:
        reply = "no reply: %s" % ex
    took = time.time() - t0
    check(reply == "T02", "^C stops a guest that waits for a key with T02 (%s)" % reply)
    check(took < 2.0, "and it takes %.2f s, not seconds more" % took)
    check(r.reg(5) != 0, "PC is valid after the stop")
    status = e.kill()
    check(status == 1, "k ends romwbw_emu with status 1 (got %s)" % status)


def case_detach(rom, disk):
    e = Emu(rom, disk)
    r = e.rsp
    r.send_packet("D")
    check(r.recv() == "OK", "D answers OK")
    status = e.finish(60)
    check(status == 0, "after D the program runs on and romwbw_emu exits 0 (got %s)" % status)
    check("GDBDONE" in e.text(), "after D the program ran to its end")


def case_limit(rom, disk):
    e = Emu(rom, disk, extra=["--max-instructions=3000"])
    r = e.rsp
    # --max-instructions comes later on the command line than ours: the last wins.
    r.send_packet("c")
    reply = r.recv(60)
    check(reply == "W7c", "the instruction limit gives W7c, status 124 (got %s)" % reply)
    status = e.finish()
    check(status == 124, "romwbw_emu exits 124 (got %s)" % status)


def case_halt(rom, work):
    """A HLT under the debugger stops with T05; a later continue ends the run."""
    work = os.path.join(work, "halt")
    os.makedirs(work)
    src = os.path.join(work, "halt.asm")
    with open(src, "w") as f:
        f.write("\t.z80\n\tld\ta,1\n\thalt\n\tld\ta,2\n\tjp\t0\n\tend\n")
    rel = os.path.join(work, "halt.rel")
    com = os.path.join(work, "halt.com")
    subprocess.run(["um80", "-o", rel, src], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["ul80", "-o", com, rel], check=True, stdout=subprocess.DEVNULL)
    data = open(com, "rb").read()
    check(data[:3] == bytes([0x3E, 0x01, 0x76]), "halt.com links to ld a,1 / halt")
    disk = disk_with(load_batch_module(), work, com)
    e = Emu(rom, disk, keys=b"HALT\r")
    r = e.rsp
    r.monitor("wait-program bytes 3e 01 76")
    r.send_packet("c")
    reply = r.recv(60)
    check(reply == "T05", "halt.com is found at 0100H, with a plain T05 (%s)" % reply)
    r.send_packet("c")
    reply = r.recv(60)
    check(reply.startswith("T05"), "HLT stops with T05 (%s)" % reply)
    check(r.reg(5) == 0x0103, "PC is 0103H after the HLT (%04X)" % r.reg(5))
    r.send_packet("c")
    reply = r.recv(60)
    check(reply == "W00", "the next continue ends the run with W00 (%s)" % reply)
    e.finish()


def case_sim_console(rom, disk):
    try:
        import pty
    except ImportError:
        print("SKIP: sim> with the stub: no pty module here")
        return
    work = tempfile.mkdtemp(prefix="gdbtest-pty-")
    portfile = os.path.join(work, "port")
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(EMU, [EMU, "--romwbw=" + rom, "--disk0=" + disk, "--no-config",
                       "--gdb=0", "--gdb-port-file=" + portfile])
    # The pty must be read all the time: the program writes a screenful before
    # it listens, and blocks when nobody takes it.
    seen = []

    def drain():
        while True:
            try:
                data = os.read(fd, 65536)
            except OSError:
                return
            if not data:
                return
            seen.append(data)

    threading.Thread(target=drain, daemon=True).start()

    def since(mark):
        return b"".join(seen[mark:]).decode("latin-1").replace("\r", "")

    deadline = time.time() + 30
    port = None
    while time.time() < deadline and port is None:
        try:
            port = int(open(portfile).read().strip())
        except (OSError, ValueError):
            time.sleep(0.05)
    if port is None:
        check(False, "sim> with the stub: the port file appeared")
        os.kill(pid, 9)
        return
    r = Rsp(port)
    check(r.cmd("?") == "T05", "on a pty the stub accepts the connection")
    r.send_packet("c")
    time.sleep(3.0)
    mark = len(seen)
    os.write(fd, b"\x05")
    time.sleep(1.0)
    check("sim> " in since(mark), "^E reaches sim> while the stub runs the program")
    mark = len(seen)
    os.write(fd, b"dm FFF0 3\n")
    time.sleep(0.7)
    out = since(mark)
    check("D3 EF C9" in out, "sim> dm shows the HBIOS entry stub (%r)" % out[-60:])
    os.write(fd, b"q\n")
    try:
        reply = r.recv(15)
    except Exception as ex:
        reply = "no reply: %s" % ex
    check(reply == "W00", "quit in sim> ends the run and the debugger gets W00 (%s)" % reply)
    try:
        os.waitpid(pid, 0)
    except OSError:
        pass
    r.close()
    shutil.rmtree(work, ignore_errors=True)


def main():
    if not os.access(EMU, os.X_OK):
        print("SKIP: gdb stub: src/romwbw_emu is not built")
        return 0
    probe = subprocess.run([EMU, "--gdb=0", "/nonexistent.rom"], stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=30)
    if b"no GDB stub" in probe.stderr:
        print("SKIP: gdb stub: this romwbw_emu was built without the stub "
              "(make could not find cpmemu's gdbstub.cc; set CPMEMU_SRC)")
        if os.environ.get("ROMWBW_REQUIRE_GDB"):
            print("FAIL: ROMWBW_REQUIRE_GDB is set, so a build without the stub fails")
            return 1
        return 0
    try:
        rb = load_batch_module()
        rom = rb.resolve_asset("@rom", offline=True)
        rb.resolve_asset(rb.DEFAULT_SYSTEM_DISK, offline=True)
    except BaseException as e:  # SystemExit from a tool means "not cached"
        print("SKIP: gdb stub: no cached ROM or hd1k_cpm22 (%s)" % (e,))
        return 0

    work = tempfile.mkdtemp(prefix="gdbtest-build-")
    try:
        try:
            com, com_bytes = build_program(work)
        except (RuntimeError, subprocess.CalledProcessError) as e:
            check(False, "build tests/gdb_prog.asm: %s" % e)
            return 1
        check(com_bytes[0:5] == bytes([0x0E, 0x09, 0x11, 0x1E, 0x01]) and
              com_bytes[A_CALL_OUTER_RET - 0x100 - 3] == 0xCD and
              com_bytes[A_CALL_INNER_RET - 0x100 - 3] == 0xCD and
              com_bytes[A_INNER - 0x100] == 0x3E,
              "gdb_prog.asm links to the offsets that this test uses")
        disk = disk_with(rb, work, com)

        case_connect(rom, disk)
        case_listen(rom, disk)
        case_program(rom, disk, com_bytes, "bytes")
        case_program(rom, disk, com_bytes, "sha256")
        case_wrong_program(rom, disk, com_bytes)
        case_interrupt(rom, disk)
        case_detach(rom, disk)
        case_limit(rom, disk)
        case_halt(rom, work)
        case_sim_console(rom, disk)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if failures:
        print("\n%d FAILED" % len(failures))
        return 1
    print("\nALL PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
