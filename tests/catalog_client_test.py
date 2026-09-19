#!/usr/bin/env python3
"""catalog_client_test.py - tools/romwbw-get against a fixture catalog, offline.

tools/romwbw-get is now the only way to get a ROM, so it is on the path between
a fresh clone and a working emulator.  This drives it end to end against a
catalog served from a local http.server, with no network and no romwbw_disks
checkout: the fixtures are built here, with the field names the real published
documents use, and small synthetic assets so the whole run takes a second.

What it is looking for, in rough order of how much it would cost to get wrong:

  * a hash or size that does not match must never reach the caller, and must
    leave nothing behind under the real filename
  * the per-version catalog must be verified BEFORE it is parsed
  * a network failure must exit 2 (cannot verify), not 1 (contradiction) - CI
    turns the first into a warning and the second into a red build
  * the cache must stay pristine and the emulator must be handed a writable
    copy, because the guest writes to its disks
  * a cached file that no longer matches must be repaired only when the
    catalog's `generation` moved; otherwise it is damage and must be reported
  * defaults must key on roms[].default and disks[].defaultSlot - never on a
    `default` field in disks[], which no published catalog has
  * unknown fields, new entries and a disappearing disk must all be tolerated

Run: python3 tests/catalog_client_test.py     (from anywhere)
     make -C src test
"""

import hashlib
import http.server
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
GET = os.path.join(HERE, os.pardir, "tools", "romwbw-get")

EX_OK, EX_CONTRADICTION, EX_UNVERIFIED, EX_USAGE = 0, 1, 2, 64

failures = []


def check(ok, what):
    print(("PASS" if ok else "FAIL") + ": " + what)
    if not ok:
        failures.append(what)


# --- a catalog to serve --------------------------------------------------------

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
# The script has no .py suffix, so it is loaded by path rather than imported.
# Only fnv1a32 is taken: the rest is exercised through the CLI, as a user meets
# it, and importing more would start testing internals instead of behaviour.
import importlib.machinery as _im
import importlib.util as _iu
_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "..", "tools", "romwbw-get")
_spec = _iu.spec_from_loader("romwbw_get",
                             _im.SourceFileLoader("romwbw_get", _path))
_rg = _iu.module_from_spec(_spec)
_spec.loader.exec_module(_rg)
fnv1a32 = _rg.fnv1a32
index_scope = _rg.index_scope
default_index_url = _rg.INDEX_URL


def assets_dir(cache, version, index_url):
    """Where this cache keeps <version>'s assets for this index URL.

    Namespaced: only the compiled-in default index is unscoped, and these tests
    serve their own, so every path here carries an @<hash> suffix.  Computed
    rather than written out, so the suite does not have to be edited again if
    the scheme changes.
    """
    # index_scope(), not a second copy of the rule: a helper that computed the
    # namespace its own way would keep passing if the production rule changed,
    # which is the opposite of what a test is for.
    return os.path.join(cache, "v0", version + index_scope(index_url), "assets")


def sha(b):
    return hashlib.sha256(b).hexdigest()


class Fixture(object):
    """Builds the two-level document set and the assets it names."""

    def __init__(self, root, base=""):
        self.root = root
        self.base = base
        self.assets = {}          # path under root -> bytes

    def rom(self, ident, version, default=False, body=None):
        name = "%s-v0-%s.rom" % (ident, version)
        body = body if body is not None else (ident + version).encode() * 64
        self.assets["v0-romwbw-%s/%s" % (version, name)] = body
        d = {"id": ident, "filename": name, "name": ident.upper(),
             "description": "a fixture ROM", "size": len(body),
             "sha256": sha(body),
             "hcb": {"marker": "57 A8", "version": "0x36", "update": "0x00"},
             "built_from": {"bank0": "src/emu_hbios.asm"}}
        if default:
            d["default"] = True
        return d

    def disk(self, ident, version, slot=None, host_transfer=False, body=None):
        name = "%s-v0-%s.img" % (ident, version)
        body = body if body is not None else (ident + version).encode() * 128
        self.assets["v0-romwbw-%s/%s" % (version, name)] = body
        d = {"id": ident, "filename": name, "name": ident.replace("_", " "),
             "description": "a fixture disk", "size": len(body),
             "sha256": sha(body), "license": "Mixed", "format": "hd1k",
             "bootable": True, "cbios": "CBIOS v%s [WBW]" % version,
             "host_transfer": host_transfer, "upstream": "Binary/%s" % name}
        if slot is not None:
            d["defaultSlot"] = slot
        return d

    def catalog(self, version, roms, disks, generation=1, base=None):
        return {
            "schema": "romwbw-disks-catalog", "schema_version": 1,
            "interface": "v0", "romwbw_version": version,
            "generation": generation, "status": "stable",
            "release_tag": "v0-romwbw-" + version,
            "base_url": (base or self.base) + "v0-romwbw-%s/" % version,
            "hbios": {"ver_byte": "0x36", "upd_byte": "0x00"},
            "upstream": {"tag": "v" + version,
                         "package_url": self.base + "Package.zip",
                         "package_sha256": sha(b"package")},
            "notes": ["a fixture"],
            "roms": roms, "disks": disks,
        }

    def write(self, base, versions, index_extra=None, catalog_mangle=None):
        """versions: [(ver, catalog, default_bool, generation[, prerelease])]

        The fifth field is optional because the real index omits the key on
        a released version - it is emitted only when true, so a released
        catalog stays byte-identical to the one on its immutable tag.  A
        fixture that always wrote `prerelease: false` would be testing a
        document romwbw_disks does not publish, and would hide exactly the
        bug .get() is there to prevent.
        """
        self.base = base
        entries = []
        for row in versions:
            ver, cat, is_default, gen = row[:4]
            pre = row[4] if len(row) > 4 else False
            raw = json.dumps(cat, indent=2).encode()
            if catalog_mangle:
                raw = catalog_mangle(ver, raw)
            path = os.path.join(self.root, "v0-romwbw-%s" % ver,
                                "catalog-v0-%s.json" % ver)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(raw)
            entries.append({
                "romwbw_version": ver, "label": "RomWBW " + ver,
                "status": "stable", "default": is_default,
                "released": "2026-01-01",
                "hbios": {"major": 3, "minor": 6, "update": 0, "patch": 0,
                          "ver_byte": "0x36", "upd_byte": "0x00",
                          "sysver_de": "0x3600"},
                "release_tag": "v0-romwbw-" + ver,
                "catalog_url": base + "v0-romwbw-%s/catalog-v0-%s.json" % (ver, ver),
                "catalog_sha256": sha(json.dumps(cat, indent=2).encode()),
                "catalog_size": len(json.dumps(cat, indent=2).encode()),
                "generation": gen,
                "rom_count": len(cat["roms"]), "disk_count": len(cat["disks"]),
                "notes": [],
            })
            if pre:
                entries[-1]["prerelease"] = True
                entries[-1]["label"] = ("RomWBW " + ver +
                                        " (development snapshot)")
                entries[-1]["status"] = "snapshot"
        index = {"schema": "romwbw-disks-index", "schema_version": 1,
                 "interface": "v0", "repo": "http://fixture",
                 "index_url": base + "index-v0.json",
                 "romwbw_versions": entries}
        if index_extra:
            index.update(index_extra)
        with open(os.path.join(self.root, "index-v0.json"), "w") as f:
            json.dump(index, f, indent=2)
        for rel, body in self.assets.items():
            p = os.path.join(self.root, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(body)


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass


def serve(directory):
    handler = lambda *a, **k: Quiet(*a, directory=directory, **k)
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, "http://127.0.0.1:%d/" % srv.server_address[1]


# --- running the thing ---------------------------------------------------------

def run(cache, base, *args, **kw):
    argv = [sys.executable, GET, "--index-url", base + "index-v0.json",
            "--cache", cache, "--retries", "0"]
    # `supported=` is accepted and dropped.  It used to become
    # --assume-supported, which steered the compile-time release allowlist
    # romwbw-get no longer has.
    kw.pop("supported", None)
    argv += list(args)
    env = dict(os.environ)
    env["XDG_CONFIG_HOME"] = kw.pop("confdir", os.path.join(cache, "conf"))
    env["XDG_DATA_HOME"] = kw.pop("datadir", os.path.join(cache, "data"))
    env.update(kw.pop("env_extra", {}))
    env.pop("ROMWBW_VERSION", None)
    env.pop("ROMWBW_INDEX_URL", None)
    env.pop("ROMWBW_GET_CACHE", None)
    return subprocess.run(argv, capture_output=True, text=True, env=env,
                          timeout=120)


def main():
    tmp = tempfile.mkdtemp(prefix="romwbw-get-test.")
    try:
        _run_all(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("-" * 64)
    if failures:
        print("%d check(s) failed" % len(failures))
        return 1
    print("all checks passed")
    return 0


def _run_all(tmp):
    web = os.path.join(tmp, "web")
    os.makedirs(web)
    srv, base = serve(web)
    try:
        _tests(tmp, web, base)
    finally:
        srv.shutdown()


def _tests(tmp, web, base):
    fx = Fixture(web)
    fx.base = base
    c351 = fx.catalog("3.5.1",
                      [fx.rom("emu_avw", "3.5.1", default=True),
                       fx.rom("emu_rcz80", "3.5.1")],
                      [fx.disk("hd1k_combo", "3.5.1", slot=0, host_transfer=True),
                       fx.disk("hd1k_ws4", "3.5.1")],
                      generation=1)
    c360 = fx.catalog("3.6.0",
                      [fx.rom("emu_avw", "3.6.0", default=True),
                       fx.rom("emu_rcz80", "3.6.0")],
                      [fx.disk("hd1k_combo", "3.6.0", slot=0, host_transfer=True),
                       fx.disk("hd1k_msx", "3.6.0")],
                      generation=2)
    fx.write(base, [("3.5.1", c351, False, 1), ("3.6.0", c360, True, 2)])

    cache = os.path.join(tmp, "cache")

    # 1. versions lists everything published ------------------------------------
    r = run(cache, base, "versions")
    check(r.returncode == EX_OK and "3.5.1" in r.stdout and "3.6.0" in r.stdout,
          "versions lists every published release")
    check("index default" in r.stdout,
          "and marks the one the index nominates")
    check("NOT RUNNABLE" not in r.stdout and "cannot run" not in r.stdout,
          "and nothing is hidden or marked unrunnable - every release a v0 "
          "catalog publishes speaks the same HBIOS-to-emulator interface")

    # 2. selection precedence ---------------------------------------------------
    r = run(cache, base, "list", "--roms")
    check("RomWBW 3.6.0" in r.stdout,
          "with nothing stored, the index default is selected")

    # 3. a healthy fetch --------------------------------------------------------
    r = run(cache, base, "fetch", "@rom")
    got = r.stdout.strip()
    check(r.returncode == EX_OK and got.endswith("emu_avw-v0-3.6.0.rom"),
          "@rom resolves to the roms[] entry flagged default:true")
    check(os.path.exists(got) and
          sha(open(got, "rb").read()) == c360["roms"][0]["sha256"],
          "and the bytes on disk are the ones the catalog names")
    check(oct(os.stat(got).st_mode & 0o777) == "0o444",
          "a cached asset is read-only: it is the hash-pinned copy, and the "
          "guest must never be able to write through to it")

    r = run(cache, base, "fetch", "@disk0")
    check(r.returncode == EX_OK and r.stdout.strip().endswith("hd1k_combo-v0-3.6.0.img"),
          "@disk0 keys on defaultSlot, which is the only default signal a "
          "published disks[] entry actually carries")
    r = run(cache, base, "fetch", "@disk1")
    check(r.returncode == EX_CONTRADICTION and "defaultSlot 1" in r.stderr,
          "@disk1 fails loudly rather than silently picking something")

    r = run(cache, base, "fetch", "no_such_disk")
    check(r.returncode == EX_CONTRADICTION and "hd1k_combo" in r.stderr,
          "an unknown id is refused, and the message lists what exists")

    # 4. a writable working copy ------------------------------------------------
    datadir = os.path.join(tmp, "data")
    r = run(cache, base, "path", "--work", "@disk0", datadir=datadir)
    work = r.stdout.strip()
    pristine = os.path.join(assets_dir(cache, "3.6.0", base + "index-v0.json"),
                            "hd1k_combo-v0-3.6.0.img")
    check(work != pristine and os.path.exists(work),
          "path --work hands back a copy, not the cached original")
    check(os.access(work, os.W_OK),
          "and that copy is writable - hbios_dispatch.cc opens every disk rw, "
          "so a CP/M SAVE would otherwise break the hash of a verified file")
    with open(work, "r+b") as f:          # what a guest write looks like
        f.write(b"MODIFIED")
    r = run(cache, base, "verify")
    check(r.returncode == EX_OK and "0 bad" in r.stdout,
          "writing to the working copy leaves the verified cache intact")

    # 5. a corrupted asset ------------------------------------------------------
    bad = Fixture(os.path.join(tmp, "bad"))
    os.makedirs(bad.root)
    badsrv, badbase = serve(bad.root)
    bad.base = badbase
    try:
        cbad = bad.catalog("3.6.0", [bad.rom("emu_avw", "3.6.0", default=True)],
                           [bad.disk("hd1k_combo", "3.6.0", slot=0)])
        bad.write(badbase, [("3.6.0", cbad, True, 1)])
        # Serve different bytes than the catalog promises - the SAME NUMBER of
        # them, so this exercises the hash rather than the cheap size check.
        p = os.path.join(bad.root, "v0-romwbw-3.6.0", "emu_avw-v0-3.6.0.rom")
        n = cbad["roms"][0]["size"]
        with open(p, "wb") as f:
            f.write(b"X" * n)
        bcache = os.path.join(tmp, "bad-cache")
        r = run(bcache, badbase, "fetch", "@rom")
        check(r.returncode == EX_CONTRADICTION and "sha256" in r.stderr,
              "an asset whose sha256 does not match is a contradiction, exit 1")
        landed = os.path.join(assets_dir(bcache, "3.6.0", badbase + "index-v0.json"),
                              "emu_avw-v0-3.6.0.rom")
        check(not os.path.exists(landed),
              "and nothing is left behind under the real filename")
        check(".bad" in r.stderr,
              "the bytes that did not verify are kept where they can be looked at")

        # And the cheap size check, which must fire before a hash is computed.
        with open(p, "wb") as f:
            f.write(b"X" * (n - 1))
        r = run(bcache, badbase, "fetch", "@rom")
        check(r.returncode == EX_CONTRADICTION and "bytes" in r.stderr,
              "a short asset is caught by size, with the count in the message")

        # A catalog document that does not match the hash the index carries
        # must be refused before it is parsed.
        bad2 = Fixture(os.path.join(tmp, "bad2"))
        os.makedirs(bad2.root)
        s2, b2 = serve(bad2.root)
        bad2.base = b2
        try:
            c2 = bad2.catalog("3.6.0", [bad2.rom("emu_avw", "3.6.0", default=True)],
                              [bad2.disk("hd1k_combo", "3.6.0", slot=0)])
            bad2.write(b2, [("3.6.0", c2, True, 1)],
                       catalog_mangle=lambda v, raw: raw + b"\n")
            r = run(os.path.join(tmp, "bad2-cache"), b2, "list")
            check(r.returncode == EX_CONTRADICTION and
                  ("sha256" in r.stderr or "bytes" in r.stderr),
                  "a catalog that does not match the index's hash is refused")
            check("re-published" in r.stderr or "index says" in r.stderr,
                  "and the message says what to do about it")
        finally:
            s2.shutdown()
    finally:
        badsrv.shutdown()

    # 5b. a 404 behind a hash-pinned document is red, not amber ----------------
    #
    # The index names a catalog_url with a sha256 and a size; a catalog names
    # every asset the same way.  A 404 on either is the release disagreeing
    # with the document that describes it - a broken publish - and it used to
    # be reported as "could not verify", which CI turns into a warning.  The
    # index's OWN url is promised by nothing, so a 404 there stays amber and
    # the cached-index fallback below still works.
    miss = Fixture(os.path.join(tmp, "miss"))
    os.makedirs(miss.root)
    msrv, mbase = serve(miss.root)
    miss.base = mbase
    try:
        cmiss = miss.catalog("3.6.0", [miss.rom("emu_avw", "3.6.0", default=True)],
                             [miss.disk("hd1k_combo", "3.6.0", slot=0)])
        miss.write(mbase, [("3.6.0", cmiss, True, 1)])
        # The catalog lists it; the release does not have it.
        os.unlink(os.path.join(miss.root, "v0-romwbw-3.6.0",
                               "emu_avw-v0-3.6.0.rom"))
        r = run(os.path.join(tmp, "miss-cache"), mbase, "fetch", "@rom")
        check(r.returncode == EX_CONTRADICTION and "404" in r.stderr,
              "an asset the catalog lists and the release does not have is a "
              "contradiction (1), not an unreachable GitHub (2)")

        # Same for the catalog document the index promises.
        os.unlink(os.path.join(miss.root, "v0-romwbw-3.6.0",
                               "catalog-v0-3.6.0.json"))
        r = run(os.path.join(tmp, "miss-cache2"), mbase, "list")
        check(r.returncode == EX_CONTRADICTION and "404" in r.stderr,
              "and so is a catalog_url the index promises that 404s")
    finally:
        msrv.shutdown()

    # 5c. an index that does not validate never replaces a good cached one -----
    bend = Fixture(os.path.join(tmp, "bendy"))
    os.makedirs(bend.root)
    bsrv, bbase = serve(bend.root)
    bend.base = bbase
    try:
        cb = bend.catalog("3.6.0", [bend.rom("emu_avw", "3.6.0", default=True)],
                          [bend.disk("hd1k_combo", "3.6.0", slot=0)])
        bend.write(bbase, [("3.6.0", cb, True, 1)])
        bcache = os.path.join(tmp, "bendy-cache")
        r = run(bcache, bbase, "list")
        check(r.returncode == EX_OK, "a good index is cached")
        # Found rather than named: a non-default index URL is namespaced, so
        # the file is index-v0@<hash>.json and hardcoding the plain name only
        # worked while nothing was scoped.
        idxdir = os.path.join(bcache, "index")
        cached = [f for f in os.listdir(idxdir)
                  if f.startswith("index-v0") and f.endswith(".json")
                  and not f.endswith(".meta.json")]
        check(len(cached) == 1, "exactly one cached index for this URL")
        idx = os.path.join(idxdir, cached[0])
        good = open(idx, "rb").read()

        # The publisher edits interface in place instead of adding a v1 beside
        # it - the exact mistake INTERFACE_V0.md's migration plan warns against.
        served = os.path.join(bend.root, "index-v0.json")
        doc = json.load(open(served))
        doc["interface"] = "v1"
        with open(served, "w") as f:
            json.dump(doc, f, indent=2)
        r = run(bcache, bbase, "--refresh", "list")
        check(r.returncode == EX_CONTRADICTION and "interface" in r.stderr,
              "an index published under an interface this build cannot speak "
              "is refused")
        check(open(idx, "rb").read() == good,
              "and the good cached index is still there, byte for byte - a bad "
              "publish does not take a working install down with it")
        r = run(bcache, bbase, "--offline", "list")
        check(r.returncode == EX_OK,
              "so an offline run still answers from the last good index")
    finally:
        bsrv.shutdown()

    # 6. network failures are exit 2, not exit 1 --------------------------------
    dead = "http://127.0.0.1:9/"     # discard port: refuses immediately
    r = run(os.path.join(tmp, "cold"), dead, "versions")
    check(r.returncode == EX_UNVERIFIED,
          "an unreachable index is CANNOT VERIFY (2), never a contradiction (1)")
    check("CANNOT VERIFY" in r.stderr, "and says so in those words")

    r = run(os.path.join(tmp, "cold2"), base, "--offline", "fetch", "@rom")
    check(r.returncode == EX_UNVERIFIED,
          "--offline with a cold cache is CANNOT VERIFY, not a failure")

    # A warm cache serves an offline run, and does not need the index again.
    r = run(cache, base, "--offline", "path", "@rom")
    check(r.returncode == EX_OK and r.stdout.strip().endswith("emu_avw-v0-3.6.0.rom"),
          "--offline against a warm cache still answers")

    # A 404 at the INDEX is different from a 404 below it: nothing promised
    # that URL, so it stays amber and a warm cache can ride it out.  This is
    # the shape of romwbw_disks cutting a release without --latest=false.
    empty = os.path.join(tmp, "no-index")
    os.makedirs(empty)
    esrv, ebase = serve(empty)
    try:
        r = run(os.path.join(tmp, "cold3"), ebase, "versions")
        check(r.returncode == EX_UNVERIFIED,
              "a 404 at the index itself is CANNOT VERIFY (2) - the entry "
              "point is promised by nothing, and a warm cache rides it out")
    finally:
        esrv.shutdown()

    # 7. cache damage vs. a moved generation ------------------------------------
    rom_path = os.path.join(assets_dir(cache, "3.6.0", base + "index-v0.json"),
                            "emu_avw-v0-3.6.0.rom")
    os.chmod(rom_path, 0o644)
    with open(rom_path, "r+b") as f:
        f.write(b"XX")
    r = run(cache, base, "path", "@rom")
    check(r.returncode == EX_CONTRADICTION and "local damage" in r.stderr,
          "a cached file that stopped matching while the catalog still "
          "publishes the same hash for it is damage, and is reported rather "
          "than silently re-downloaded over")
    r = run(cache, base, "verify")
    check(r.returncode == EX_CONTRADICTION and "FAIL" in r.stdout,
          "verify finds it too")
    r = run(cache, base, "verify", "--repair")
    check(r.returncode == EX_OK,
          "verify --repair is the explicit way to fix it")
    check(sha(open(rom_path, "rb").read()) == c360["roms"][0]["sha256"],
          "and it does")

    # Now move the generation the way a re-publish would, and change the bytes.
    fx2 = Fixture(web)
    fx2.base = base
    newrom = fx2.rom("emu_avw", "3.6.0", default=True, body=b"rebuilt" * 100)
    c360b = fx2.catalog("3.6.0", [newrom, fx2.rom("emu_rcz80", "3.6.0")],
                        [fx2.disk("hd1k_combo", "3.6.0", slot=0, host_transfer=True),
                         fx2.disk("hd1k_msx", "3.6.0")],
                        generation=3)
    fx2.write(base, [("3.5.1", c351, False, 1), ("3.6.0", c360b, True, 3)])
    r = run(cache, base, "--refresh", "path", "@rom")
    check(r.returncode == EX_OK,
          "when the generation HAS moved, a changed asset is re-fetched")
    check(sha(open(rom_path, "rb").read()) == newrom["sha256"],
          "and the new bytes are what land")
    sup = os.path.join(assets_dir(cache, "3.6.0", base + "index-v0.json"),
                       ".superseded")
    check(os.path.isdir(sup) and os.listdir(sup),
          "the bytes it replaced are kept, not deleted - a generation bump is "
          "not a licence to destroy what is on disk")

    # ...and a re-cut that changes MORE THAN ONE artifact under one generation
    # bump, which is the shape romwbw_disks actually publishes ("HB_BNKCALL
    # works, which rebuilds every ROM and bumps both generations").  The
    # release counter was stamped by the first asset repaired, so every other
    # artifact of the same re-cut was then reported to the user as local
    # damage.  The question is per-asset now.
    disk_path = os.path.join(assets_dir(cache, "3.6.0", base + "index-v0.json"),
                             "hd1k_combo-v0-3.6.0.img")
    r = run(cache, base, "path", "@disk0")
    check(r.returncode == EX_OK and os.path.exists(disk_path),
          "both artifacts of the release are cached before the re-cut")

    fx2b = Fixture(web)
    fx2b.base = base
    rom_g4 = fx2b.rom("emu_avw", "3.6.0", default=True, body=b"gen4rom" * 100)
    disk_g4 = fx2b.disk("hd1k_combo", "3.6.0", slot=0, host_transfer=True,
                        body=b"gen4disk" * 100)
    c360c = fx2b.catalog("3.6.0", [rom_g4, fx2b.rom("emu_rcz80", "3.6.0")],
                         [disk_g4, fx2b.disk("hd1k_msx", "3.6.0")],
                         generation=4)
    fx2b.write(base, [("3.5.1", c351, False, 1), ("3.6.0", c360c, True, 4)])
    r = run(cache, base, "--refresh", "fetch", "@rom", "@disk0")
    check(r.returncode == EX_OK,
          "a re-cut that changes two artifacts under one generation bump "
          "repairs BOTH, instead of calling the second one local damage")
    check(sha(open(rom_path, "rb").read()) == rom_g4["sha256"] and
          sha(open(disk_path, "rb").read()) == disk_g4["sha256"],
          "and both carry the new bytes")

    # The damage branch is still reachable after all that: same catalog, bytes
    # changed underneath.
    os.chmod(disk_path, 0o644)
    with open(disk_path, "r+b") as f:
        f.write(b"ZZ")
    r = run(cache, base, "path", "@disk0")
    check(r.returncode == EX_CONTRADICTION and "local damage" in r.stderr,
          "and damage to one artifact is still damage, not a re-publish")
    r = run(cache, base, "verify", "--repair")
    check(r.returncode == EX_OK, "verify --repair still fixes it")

    # 8. a release disappearing from a version, and unknown fields --------------
    fx3 = Fixture(web)
    fx3.base = base
    c360c = fx3.catalog("3.6.0",
                        [fx3.rom("emu_avw", "3.6.0", default=True)],
                        [fx3.disk("hd1k_combo", "3.6.0", slot=0, host_transfer=True)],
                        generation=4)
    # A field no version of this client has ever heard of, at both levels.
    c360c["something_new"] = {"added": "later"}
    c360c["roms"][0]["signature"] = "deadbeef"
    fx3.write(base, [("3.6.0", c360c, True, 4)],
              index_extra={"future_field": [1, 2, 3]})
    r = run(cache, base, "--refresh", "list")
    check(r.returncode == EX_OK and "emu_avw" in r.stdout,
          "unknown fields at both levels are ignored, not rejected")
    check("hd1k_msx" not in r.stdout,
          "a disk that is no longer published simply is not offered")
    r = run(cache, base, "--refresh", "fetch", "hd1k_msx")
    check(r.returncode == EX_CONTRADICTION,
          "and asking for it by id is a clear refusal")

    # A published release that vanished from the index is a hard error, not a
    # silent fallback to something else.
    r = run(cache, base, "--refresh", "--romwbw", "3.5.1", "list")
    check(r.returncode == EX_CONTRADICTION and "does not publish" in r.stderr,
          "a release the index has stopped publishing is refused by name")

    # 9. an obsolete gate flag is accepted and ignored --------------------------
    #
    # --assume-supported and --allow-untested steered a compile-time release
    # allowlist that no longer exists.  Installed scripts still pass them, so
    # they must not become "unrecognized arguments" and exit 64.
    plain = run(cache, base, "--refresh", "versions")
    flagged = run(cache, base, "--refresh", "--assume-supported", "9.9.9",
                  "--allow-untested", "versions")
    check(flagged.returncode == EX_OK,
          "the retired gate flags are accepted, not an argparse error")
    # "9.9.9" is published by nothing.  While the allowlist existed this
    # steered selection and would have emptied the listing; it must now be
    # inert, so the two runs have to agree byte for byte.
    check(flagged.stdout == plain.stdout,
          "and they are inert - the listing is identical with and without them")
    r = run(cache, base, "--assume-supported", "9.9.9", "fetch", "@rom")
    check(r.returncode == EX_OK,
          "and a fetch still resolves a release they claim is unsupported")

    # 10. the web mirror --------------------------------------------------------
    site = os.path.join(tmp, "site")
    r = run(cache, base, "--refresh", "mirror", site, "--versions", "all")
    man = os.path.join(site, "catalog", "manifest.json")
    check(r.returncode == EX_OK and os.path.exists(man),
          "mirror writes a manifest beside the page")
    doc = json.load(open(man))
    check(doc["schema"] == "romwbw-emu-mirror" and doc["interface"] == "v0",
          "and it identifies itself")
    block = doc["romwbw_versions"][0]
    rel = block["roms"][0]["url"]
    check(not rel.startswith("http") and
          os.path.exists(os.path.join(site, "catalog", rel)),
          "every url in it is mirror-relative and the file is really there - "
          "a browser cannot follow the catalog's own base_url")
    check(block["roms"][0]["sha256"] and block["roms"][0]["size"],
          "the size and hash travel with it, so the page can check what it got")
    check("emu_supported" not in doc,
          "and no longer records a release allowlist for the page to grey "
          "options out with")

    r = run(cache, base, "mirror", site, "--versions", "all", "--only", "emu_avw")
    doc = json.load(open(man))
    block = doc["romwbw_versions"][0]
    check(len(block["disks"]) == 0 and block["not_mirrored"],
          "a partial mirror records what it left out rather than looking complete")

    # A re-cut does NOT change an artifact's size - a ROM is always 512 KB, an
    # hd1k image always exactly 8 MB - so a mirror that decided by size skipped
    # the copy and then wrote the catalog's NEW sha256 into the manifest beside
    # LAST generation's bytes.  The page checks what it downloads against that
    # manifest, so the deploy exited 0 and published a web root that failed its
    # own integrity check.
    fx4 = Fixture(web)
    fx4.base = base
    recut = fx4.rom("emu_avw", "3.6.0", default=True,
                    body=b"Y" * len(b"gen4rom" * 100))  # same SIZE as before
    c360d = fx4.catalog("3.6.0", [recut, fx4.rom("emu_rcz80", "3.6.0")],
                        [fx4.disk("hd1k_combo", "3.6.0", slot=0,
                                  host_transfer=True, body=b"gen4disk" * 100),
                         fx4.disk("hd1k_msx", "3.6.0")],
                        generation=5)
    fx4.write(base, [("3.5.1", c351, False, 1), ("3.6.0", c360d, True, 5)])
    r = run(cache, base, "--refresh", "mirror", site, "--versions", "all")
    check(r.returncode == EX_OK, "a mirror after a re-cut succeeds")
    doc = json.load(open(man))
    # Both releases publish an id "emu_avw"; pick the block for the one that
    # was re-cut rather than whichever the index happens to list first.
    mblock = [b for b in doc["romwbw_versions"]
              if b["romwbw_version"] == "3.6.0"][0]
    ment = [e for e in mblock["roms"] if e["id"] == "emu_avw"][0]
    on_disk = open(os.path.join(site, "catalog", *ment["url"].split("/")), "rb").read()
    check(ment["sha256"] == recut["sha256"],
          "the manifest carries the re-cut's new hash")
    check(sha(on_disk) == ment["sha256"],
          "and the bytes beside it are the ones that hash to it - a same-size "
          "re-cut is re-copied, not skipped")

    # 10b. mirror keeps the 1/2 split --------------------------------------------
    #
    # A full mirror pulls every asset of every release - hundreds of megabytes.
    # One 5xx or reset in that is a GitHub hiccup, not a defect in this
    # repository, and it used to come out as exit 1 alongside real
    # contradictions, so a deploy step went red for something nobody here
    # could fix.
    dead_assets = "http://127.0.0.1:9/"      # discard port: refuses at once
    fx5 = Fixture(web)
    fx5.base = base
    c_dead = fx5.catalog("3.6.0", [fx5.rom("emu_avw", "3.6.0", default=True)],
                         [fx5.disk("hd1k_combo", "3.6.0", slot=0)],
                         generation=6, base=dead_assets)
    fx5.write(base, [("3.6.0", c_dead, True, 6)])
    site2 = os.path.join(tmp, "site2")
    r = run(os.path.join(tmp, "mcache"), base, "mirror", site2, "--versions", "all")
    check(r.returncode == EX_UNVERIFIED,
          "a mirror whose assets are unreachable is CANNOT VERIFY (2), not a "
          "contradiction (1) - a transient GitHub failure is not a defect here")
    check(os.path.exists(os.path.join(site2, "catalog", "manifest.json")),
          "and it still writes the manifest for what it did get")

    # A mirror that hits a real contradiction is still red.
    fx6 = Fixture(web)
    fx6.base = base
    c_gone = fx6.catalog("3.6.0", [fx6.rom("emu_avw", "3.6.0", default=True)],
                         [fx6.disk("hd1k_combo", "3.6.0", slot=0)],
                         generation=7)
    fx6.write(base, [("3.6.0", c_gone, True, 7)])
    os.unlink(os.path.join(web, "v0-romwbw-3.6.0", "emu_avw-v0-3.6.0.rom"))
    r = run(os.path.join(tmp, "mcache2"), base, "mirror",
            os.path.join(tmp, "site3"), "--versions", "all")
    check(r.returncode == EX_CONTRADICTION,
          "but an asset the catalog lists and the release lacks is still red")

    # 10c. a non-default index gets its own namespace ---------------------------
    #
    # Without this, a fork's index was written over the real one's cache entry
    # and its assets collided by filename: within INDEX_TTL a plain run
    # afterwards was served the fork's index, and a same-named asset left the
    # next default run reporting local damage on a file it had never touched.
    # `--cache DIR` was the workaround; this asserts it is not needed.
    #
    # Two fixtures of its own rather than the suite's shared one, which earlier
    # tests have deliberately damaged.
    two = {}
    for name, body in (("alpha", b"ALPHA!!"), ("beta", b"BETA!!!")):
        fx = Fixture(os.path.join(tmp, "ns-" + name))
        os.makedirs(fx.root)
        srv, b = serve(fx.root)
        fx.base = b
        rom = fx.rom("emu_avw", "3.6.0", default=True, body=body * 512)
        fx.write(b, [("3.6.0", fx.catalog("3.6.0", [rom],
                      [fx.disk("hd1k_combo", "3.6.0", slot=0,
                               body=body * 1024)]), True, 1)])
        two[name] = (b, srv, body * 512)
    try:
        shared = os.path.join(tmp, "ns-cache")
        for name in ("alpha", "beta"):
            r = run(shared, two[name][0], "--refresh", "fetch", "@rom", "@disk0")
            check(r.returncode == EX_OK,
                  "index %s fetches into the shared cache root" % name)

        roms = []
        for dp, _dirs, fs in os.walk(shared):
            roms += [os.path.join(dp, f) for f in fs if f.endswith(".rom")]
        check(len(roms) == 2,
              "both ROMs are kept - one namespace per index, not one file "
              "overwritten by the other")
        bodies = set()
        for f in roms:
            with open(f, "rb") as fh:
                bodies.add(fh.read())
        check(bodies == {two["alpha"][2], two["beta"][2]},
              "and they are the two different bodies, so neither index "
              "superseded the other's asset")

        idx = [f for f in os.listdir(os.path.join(shared, "index"))
               if f.endswith(".json") and not f.endswith(".meta.json")]
        check(len(idx) == 2,
              "two index documents, so a cached index can never be served to a "
              "run that asked for a different one")

        r = run(shared, two["beta"][0], "path", "@rom")
        check("@" in r.stdout.strip().splitlines()[-1],
              "a non-default index gets an @<hash> suffix on the release "
              "directory")
        check("separate from the default" in r.stderr,
              "and the warning says so, rather than leaving it to be inferred")

        works = set()
        for name in ("alpha", "beta"):
            r = run(shared, two[name][0], "path", "--work", "@disk0")
            works.add(r.stdout.strip().splitlines()[-1])
        check(len(works) == 2,
              "a fork's disk image does not write over the working copy the "
              "user has been saving into")
    finally:
        for name in ("alpha", "beta"):
            two[name][1].shutdown()

    # THE DEFAULT INDEX IS UNSCOPED, which is what keeps an existing cache
    # working and means nothing has to be migrated.  Asserted on the rule rather
    # than through the CLI, because reaching the real default URL would need the
    # network and this suite has none.
    check(index_scope(default_index_url) == "",
          "the compiled-in default index has the empty scope, so its paths are "
          "exactly what they have always been")
    check(index_scope("https://example.invalid/index-v0.json") == "@017dbcf1",
          "and any other URL gets an @<hash> of its own")

    # The hash is shared with the three GUI clients; a drift here would put the
    # same catalog in two different namespaces depending on which port you used.
    check(fnv1a32("") == "4fd0bfc1" and
          fnv1a32("https://example.invalid/index-v0.json") == "017dbcf1",
          "fnv1a32 matches ioscpm's Swift and cpmdroid's Kotlin, verified by "
          "compiling and running all three")

    # 11. `use` persists a choice ------------------------------------------------
    conf = os.path.join(tmp, "conf")
    r = run(cache, base, "--refresh", "use", "3.6.0", confdir=conf)
    check(r.returncode == EX_OK, "use stores a release")
    stored = json.load(open(os.path.join(conf, "romwbw_emu", "catalog.json")))
    check(stored["romwbw_version"] == "3.6.0" and stored["interface"] == "v0",
          "in its own file, scoped to the interface version")
    check("rom" not in stored or stored.get("rom") is None,
          "and stores only what was asked for")
    r = run(cache, base, "use", "--clear", confdir=conf)
    check(r.returncode == EX_OK and
          not os.path.exists(os.path.join(conf, "romwbw_emu", "catalog.json")),
          "use --clear forgets it")

    # 11b. a stored release that stops being usable must SAY so --------------
    #
    # The stored value is durable and the world around it is not: an index can
    # stop publishing a release.  That does not rewrite catalog.json, and until
    # this was fixed it said nothing either - the file named a release for ever
    # while every run fetched something else, and the only place it showed was
    # `versions --json`.  What is asserted here is the SAYING, not a change of
    # choice: the file is still left exactly as the user left it.
    #
    # There used to be a second way in, and it is gone: a rebuilt binary could
    # stop RUNNING a release, because the core carried a compile-time release
    # allowlist.  Nothing has that opinion now, so the index is the only thing
    # that can strand a stored choice.
    #
    # On its own index and its own cache.  The fixture above has been rewritten
    # several times by the sections in between, and a test of what is stored
    # against what is published has to control both.
    keep = Fixture(os.path.join(tmp, "keep"))
    os.makedirs(keep.root)
    s4, b4 = serve(keep.root)
    kconf = os.path.join(tmp, "keep-conf")
    kcache = os.path.join(tmp, "keep-cache")
    try:
        keep.base = b4
        k351 = keep.catalog("3.5.1", [keep.rom("emu_avw", "3.5.1", default=True)],
                            [keep.disk("hd1k_combo", "3.5.1", slot=0)])
        k360 = keep.catalog("3.6.0", [keep.rom("emu_avw", "3.6.0", default=True)],
                            [keep.disk("hd1k_combo", "3.6.0", slot=0)])
        keep.write(b4, [("3.5.1", k351, False, 1), ("3.6.0", k360, True, 1)])

        r = run(kcache, b4, "use", "3.6.0", confdir=kconf)
        check(r.returncode == EX_OK, "a release is stored")

        # A stored release the index still publishes is simply used, with no
        # warning and no divergence - the case a retired binary allowlist used
        # to be able to break.
        r = run(kcache, b4, "list", "--roms", confdir=kconf)
        check(r.returncode == EX_OK and "RomWBW 3.6.0" in r.stdout,
              "a stored release the index publishes is the one used")
        check("being used instead" not in r.stderr,
              "with nothing to warn about")
        r = run(kcache, b4, "versions", "--json", confdir=kconf)
        doc = json.loads(r.stdout)
        check(doc["selected"] == "3.6.0" and doc["in_use"] == "3.6.0",
              "and --json agrees: `selected` is the file, `in_use` is what a "
              "run gets, and they match")
        check("emu_supported" not in doc,
              "and it no longer reports a release allowlist")
        stored = json.load(open(os.path.join(kconf, "romwbw_emu", "catalog.json")))
        check(stored["romwbw_version"] == "3.6.0",
              "and the stored choice is left exactly as the user left it")

        r = run(kcache, b4, "versions", confdir=kconf)
        check("catalog.json selects" not in r.stdout,
              "and says none of it when the stored release IS the one in use")
        check("NOT RUNNABLE" not in r.stdout and "cannot run" not in r.stdout,
              "and marks no row unrunnable, whatever is stored")
        # An explicit --romwbw that differs from the stored choice is honoured
        # without a warning: overriding is not divergence.
        r = run(kcache, b4, "--romwbw", "3.5.1", "list", "--roms", confdir=kconf)
        check(r.returncode == EX_OK and "RomWBW 3.5.1" in r.stdout,
              "an explicit --romwbw overrides the stored release")
        check("being used instead" not in r.stderr,
              "and overriding is not reported as a stale stored choice")

        # The other way in: the index moves.  A second one that has never heard
        # of 3.6.0, reached with the same config file.
        gone = Fixture(os.path.join(tmp, "gone"))
        os.makedirs(gone.root)
        s5, b5 = serve(gone.root)
        try:
            gone.base = b5
            g351 = gone.catalog("3.5.1",
                                [gone.rom("emu_avw", "3.5.1", default=True)],
                                [gone.disk("hd1k_combo", "3.5.1", slot=0)])
            gone.write(b5, [("3.5.1", g351, True, 1)])
            gcache = os.path.join(tmp, "gone-cache")
            r = run(gcache, b5, "list", "--roms", confdir=kconf)
            check(r.returncode == EX_OK and "does not publish" in r.stderr and
                  "3.6.0" in r.stderr,
                  "a stored release the index has dropped warns, naming it")
            r = run(gcache, b5, "versions", confdir=kconf)
            check("this index does not publish it" in r.stdout,
                  "and `versions` says so in the listing")

            # `use --index-url URL` with no version stores the URL and
            # resolves nothing against it, which is the easiest way to arrive
            # at the state above.
            r = run(gcache, b5, "use", confdir=kconf)
            check(r.returncode == EX_OK and
                  "does not publish RomWBW 3.6.0" in r.stderr,
                  "and `use --index-url` says so at the moment the index is "
                  "pointed somewhere that has never heard of the stored "
                  "release")
        finally:
            s5.shutdown()
    finally:
        s4.shutdown()

    # 12. an index this client does not speak ----------------------------------
    fut = Fixture(os.path.join(tmp, "fut"))
    os.makedirs(fut.root)
    s3, b3 = serve(fut.root)
    fut.base = b3
    try:
        cf = fut.catalog("3.6.0", [fut.rom("emu_avw", "3.6.0", default=True)], [])
        fut.write(b3, [("3.6.0", cf, True, 1)], index_extra={"interface": "v1"})
        r = run(os.path.join(tmp, "fut-cache"), b3, "versions")
        check(r.returncode == EX_CONTRADICTION and "interface" in r.stderr,
              "an index published under a newer interface is refused by name, "
              "not misread as v0")
    finally:
        s3.shutdown()

    # 13. development snapshots are opt-in --------------------------------------
    #
    # The index may carry RomWBW development snapshots, flagged
    # `prerelease: true`.  CATALOG_SCHEMA.md 2.3.1: a client MUST NOT offer one
    # by default.  The reason is that nothing downstream can tell them apart -
    # a snapshot and the release it precedes report the SAME HBIOS version
    # bytes, so emu_validate_rom_hcb cannot, and only the CBIOS banner inside
    # the disk image carries the full tag.
    pre = Fixture(os.path.join(tmp, "pre"))
    os.makedirs(pre.root)
    s6, b6 = serve(pre.root)
    try:
        pre.base = b6
        p360 = pre.catalog("3.6.0",
                           [pre.rom("emu_avw", "3.6.0", default=True)],
                           [pre.disk("hd1k_combo", "3.6.0", slot=0)])
        p370 = pre.catalog("3.7.0-dev.14",
                           [pre.rom("emu_avw", "3.7.0-dev.14", default=True)],
                           [pre.disk("hd1k_combo", "3.7.0-dev.14", slot=0)])
        # Emitted in semver precedence order, in which a pre-release sorts
        # BEFORE the release it precedes.  The snapshot is listed second here
        # and first below, because neither position may decide anything.
        pre.write(b6, [("3.6.0", p360, True, 1),
                       ("3.7.0-dev.14", p370, False, 1, True)])
        pcache = os.path.join(tmp, "pre-cache")
        pconf = os.path.join(tmp, "pre-conf")

        r = run(pcache, b6, "versions", confdir=pconf)
        check(r.returncode == EX_OK and "3.6.0" in r.stdout
              and "3.7.0-dev.14" not in r.stdout.split("not shown:")[0],
              "versions does not list a development snapshot")
        check("not shown" in r.stdout and "3.7.0-dev.14" in r.stdout,
              "but names what it held back, and the exact string to ask for - "
              "romwbw_version is a full upstream tag, not three numbers")

        r = run(pcache, b6, "--prerelease", "versions", confdir=pconf)
        check(r.returncode == EX_OK and "3.7.0-dev.14" in r.stdout,
              "--prerelease lists it")
        check("RomWBW 3.7.0-dev.14 (development snapshot)" in r.stdout,
              "showing the published label verbatim, which 2.3 says is "
              "the surface carrying the warning - this row is the whole "
              "of what tells a reader the release is not one")
        check(r.stdout.count("(development snapshot)") == 1,
              "and once, not twice: a mark of our own beside the label "
              "is a second copy of one fact")
        r = run(pcache, b6, "versions", "--all", confdir=pconf)
        check("3.7.0-dev.14" in r.stdout and "not shown" not in r.stdout,
              "and `versions --all` is the same switch: it was a no-op left "
              "from the release allowlist, and 'show every row' is what "
              "anyone still passing it means")
        r = run(pcache, b6, "versions", confdir=pconf,
                env_extra={"ROMWBW_PRERELEASE": "1"})
        check("3.7.0-dev.14" in r.stdout and "not shown" not in r.stdout,
              "ROMWBW_PRERELEASE=1 is the same switch for a shell that has "
              "already decided")
        r = run(pcache, b6, "versions", confdir=pconf,
                env_extra={"ROMWBW_PRERELEASE": "0"})
        check("not shown" in r.stdout,
              "and ROMWBW_PRERELEASE=0 reads the way anyone would expect")

        r = run(pcache, b6, "versions", "--json", confdir=pconf)
        doc = json.loads(r.stdout)
        vs = [e["romwbw_version"] for e in doc["romwbw_versions"]]
        check("3.7.0-dev.14" not in vs
              and doc["hidden_prerelease"] == ["3.7.0-dev.14"],
              "--json filters romwbw_versions and names what it held back "
              "separately - a GUI renders that array straight into a picker, "
              "which is the client 2.3.1 is written for")

        r = run(pcache, b6, "list", "--roms", confdir=pconf)
        check(r.returncode == EX_OK and "RomWBW 3.6.0" in r.stdout,
              "with nothing named, a run lands on the release")

        r = run(pcache, b6, "--romwbw", "3.7.0-dev.14", "list", "--roms",
                confdir=pconf)
        check(r.returncode == EX_OK and "3.7.0-dev.14" in r.stdout,
              "naming a snapshot exactly reaches it without --prerelease: "
              "naming it IS the opt-in")
        check("development snapshot" in r.stdout,
              "and `list` shows the published label, which is the surface "
              "carrying the warning")

        r = run(pcache, b6, "use", "3.7.0-dev.14", confdir=pconf)
        check(r.returncode == EX_OK and "development snapshot" in r.stderr,
              "`use` stores a snapshot and says what it just wrote down")
        r = run(pcache, b6, "list", "--roms", confdir=pconf)
        check(r.returncode == EX_OK and "3.7.0-dev.14" in r.stdout,
              "a stored snapshot is honoured - `use` was the explicit opt-in")
        check("development snapshot" in r.stderr,
              "and never arrives silently: this is the one path where a "
              "snapshot is selected and the user did not say so in THIS "
              "command")
        r = run(pcache, b6, "versions", confdir=pconf)
        check("3.7.0-dev.14" in r.stdout and "selected" in r.stdout,
              "and the row is shown although it is a snapshot - hiding the "
              "selected row is how a stale choice survives unnoticed")

        r = run(pcache, b6, "use", "--clear", confdir=pconf)
        check(r.returncode == EX_OK, "use --clear forgets it again")

        # The mirror is the picker the web page builds itself from:
        # fillVersionSelect() renders manifest.romwbw_versions straight into
        # the select, so mirroring a snapshot IS offering it.
        psite = os.path.join(tmp, "pre-site")
        r = run(pcache, b6, "mirror", psite, "--versions", "all", confdir=pconf)
        doc = json.load(open(os.path.join(psite, "catalog", "manifest.json")))
        vs = [e["romwbw_version"] for e in doc["romwbw_versions"]]
        check(r.returncode == EX_OK and vs == ["3.6.0"],
              "`mirror --versions all` means every RELEASE: a snapshot is not "
              "written into the manifest the web page fills its picker from")
        check("3.7.0-dev.14" in r.stdout or "3.7.0-dev.14" in r.stderr,
              "and it says which one it left out")

        psite2 = os.path.join(tmp, "pre-site2")
        r = run(pcache, b6, "--prerelease", "mirror", psite2, "--versions",
                "all", confdir=pconf)
        doc = json.load(open(os.path.join(psite2, "catalog", "manifest.json")))
        blocks = {e["romwbw_version"]: e for e in doc["romwbw_versions"]}
        check(r.returncode == EX_OK and "3.7.0-dev.14" in blocks,
              "--prerelease mirrors it")
        check(blocks["3.7.0-dev.14"].get("prerelease") is True,
              "and the block carries the flag, so a page that opts in can "
              "label it")
        check("prerelease" not in blocks["3.6.0"],
              "while a released version's block is unchanged - the key is "
              "emitted only when true, exactly as the index emits it")
        check("development snapshot" in blocks["3.7.0-dev.14"]["label"],
              "and the label travels, which is what fillVersionSelect() "
              "renders into the option text")

        # No `default` anywhere, and the snapshot FIRST.  fallback() drops to
        # pool[0] when the index nominates nothing, so this is the arrangement
        # in which a position-based pick lands on a snapshot.
        nd = Fixture(os.path.join(tmp, "nodefault"))
        os.makedirs(nd.root)
        s7, b7 = serve(nd.root)
        try:
            nd.base = b7
            n370 = nd.catalog("3.7.0-dev.2",
                              [nd.rom("emu_avw", "3.7.0-dev.2", default=True)],
                              [nd.disk("hd1k_combo", "3.7.0-dev.2", slot=0)])
            n360 = nd.catalog("3.6.0",
                              [nd.rom("emu_avw", "3.6.0", default=True)],
                              [nd.disk("hd1k_combo", "3.6.0", slot=0)])
            nd.write(b7, [("3.7.0-dev.2", n370, False, 1, True),
                          ("3.6.0", n360, False, 1)])
            r = run(os.path.join(tmp, "nd-cache"), b7, "list", "--roms",
                    confdir=os.path.join(tmp, "nd-conf"))
            check(r.returncode == EX_OK and "RomWBW 3.6.0" in r.stdout,
                  "with no index default at all, the fallback still lands on "
                  "a release - the pool a run chooses from is filtered, not "
                  "just the rows a listing prints")
        finally:
            s7.shutdown()
    finally:
        s6.shutdown()


if __name__ == "__main__":
    sys.exit(main())
