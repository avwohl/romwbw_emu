# Where the ROM and the disks come from

This repository ships **no ROM image and no disk image**. It ships the emulator,
the Z80 sources for the parts it owns, and `tools/romwbw-get`, which fetches
the artifacts from a catalog and verifies every byte of them.

    tools/romwbw-get run

That is the whole quick start: it reads the catalog, works out which RomWBW
release this build can boot, downloads that release's default ROM and disk,
checks both against the SHA-256 the catalog publishes, and starts the emulator.

## Why

Until v1.40 the ROMs and images were tracked here: six 512 KB ROMs in `roms/`,
two more copies of one of them in `web/`, and a 49 MB combo image and an 8 MB
one in `disks/` — about 61 MB of binary in a repository whose source is a few
hundred kilobytes. Every published `.deb` and `.rpm` carried 3.7 MB of ROM.

The cost was not the size. It was that **adding a disk image meant releasing
this repository**, and the three GUI clients besides. A user who wanted a disk
that had been published upstream had to wait for four application releases that
contained no change to any application.

[avwohl/romwbw_disks](https://github.com/avwohl/romwbw_disks) now builds and
publishes the ROMs and images for every supported RomWBW release, behind a
two-level catalog. Adding one is a release *there*, and it reaches everyone who
already has this installed. Nothing here has to move.

There is one exception, and it is deliberate: a new RomWBW **release** — 3.7.0,
say — still needs a rebuild here, because `ROMWBW_SUPPORTED_RELEASES` in
`src/romwbw_pin.h` is compile-time, and adding a line to it is a claim that
somebody booted that release and watched it work. `romwbw-get` refuses such a
release by name before it downloads anything - a catalog ROM is 512 KB, and
the 49 MB is the disk `run` fetches beside it.

## The shape of it

    index-v0.json                    one URL, compiled into romwbw-get
      -> catalog-v0-<release>.json   one per RomWBW release, hash named by the index
        -> the ROMs and disk images  hash and size named by the catalog

The index is fetched through `releases/latest/download/`, which names no release
tag: GitHub resolves it to whichever release carries the Latest flag, so where
the index lives belongs to romwbw_disks and can change with no release of any
client. It is a few kilobytes; the per-release catalogs and the assets live on
immutable tags and never move. So the thing that changes is tiny and the things
a client caches are permanent.

`romwbw-get` verifies in that order and no other: the catalog document is
checked against the index's `catalog_sha256` and `catalog_size` **before a byte
of it is parsed**, and each asset against its own entry. Nothing appears under
its real filename until it has passed both.

## Commands

| | |
|---|---|
| `romwbw-get versions` | which RomWBW releases are published, and which this build can run |
| `romwbw-get list` | the ROMs and disks in the selected release |
| `romwbw-get use 3.5.1` | pick a release and remember it |
| `romwbw-get fetch @rom @disk0` | download and verify, printing the paths |
| `romwbw-get path @rom` | one path, fetching if needed — for `$(...)` |
| `romwbw-get path --work @disk0` | a *writable* copy of a disk (see below) |
| `romwbw-get run` | fetch what is needed and start the emulator |
| `romwbw-get mirror DIR` | populate a same-origin directory for the web page |
| `romwbw-get verify` | re-hash every cached asset, the upstream `Package.zip` included; `--repair` re-fetches |
| `romwbw-get cache-dir` | print the cache root and exit, for a script that needs the path |

`@rom`, `@disk0`, `@disk1` and `@upstream` are pseudo-ids. `@rom` is the ROM the
catalog flags `default: true`; `@disk0` and `@disk1` are the disks it gives
`defaultSlot` 0 and 1. They exist so that nothing in this repository names
`emu_avw` or `hd1k_combo` — a client that hardcodes an id is a client that
breaks when the catalog is rearranged.

Anything after a bare `--` goes to the emulator:

    tools/romwbw-get run -- --boot=2.1 --escape=none

The flags that apply to every subcommand go **before** it, which is easy to get
wrong — `romwbw-get run --emu=...` is a usage error, `romwbw-get --emu=... run`
is not:

| | |
|---|---|
| `--index-url URL` | read a different catalog (see below) |
| `--cache DIR` | a different cache root; `$ROMWBW_GET_CACHE` does the same |
| `--offline` | never open a socket |
| `--refresh` | ignore the one-hour index cache TTL |
| `--romwbw VER` | use this RomWBW release for this run |
| `--allow-untested` | allow a release this build has not been checked against |
| `--emu PATH` | the `romwbw_emu` binary to run, and to ask what it supports |
| `--trust-cache` | check cached sizes but not their sha256 |

**The index is cached for an hour** (`INDEX_TTL`), and a cached one is used for
up to 30 days if the network is unreachable. So a newly published disk may not
appear immediately: `--refresh` is what skips the wait.

## Fetching from a fork, or from anywhere else

Nothing about the catalog is specific to `avwohl/romwbw_disks` except the
compiled-in default. Point the client at your own index and it will read yours.
There are three ways, and this is the precedence — first one set wins:

| | |
|---|---|
| `--index-url URL` | this run only. A **global** flag, so it goes *before* the subcommand |
| `$ROMWBW_INDEX_URL` | every run in that shell |
| `romwbw-get --index-url URL use <release>` | remembered, until `use --clear` |

```sh
# one run
tools/romwbw-get --index-url https://github.com/you/romwbw_disks/releases/latest/download/index-v0.json list

# this shell
export ROMWBW_INDEX_URL=https://github.com/you/romwbw_disks/releases/latest/download/index-v0.json
tools/romwbw-get run

# remembered, and how to stop
tools/romwbw-get --index-url https://.../index-v0.json use 3.6.0
tools/romwbw-get use --clear
```

It says so on every run, so you cannot forget which catalog you are on:

    romwbw-get: using a non-default index URL: https://...
    romwbw-get: its downloads are kept under ~/.cache/romwbw_emu/v0/<release>@6658478e, separate from the default's

**Each index gets its own namespace, so you do not need `--cache`.** The release
directory and the cached index carry `@` plus a hash of the URL, and so does the
writable-copy directory:

    ~/.cache/romwbw_emu/index/index-v0@6658478e.json
    ~/.cache/romwbw_emu/v0/3.6.0@6658478e/assets/
    ~/.local/share/romwbw_emu/disks/3.6.0@6658478e/

The **default** index is the empty scope, so its paths are exactly what they
have always been and an existing cache needs no migration. Before this, a fork's
index was written over the real one's cache entry and its assets collided by
filename: within the one-hour index TTL an ordinary run afterwards was served
the *fork's* index with no warning, and a same-named asset left the next default
run reporting local damage on a file it had never touched. `--cache DIR` was the
workaround and still exists, but is no longer needed for this.

The hash is FNV-1a, folded to 32 bits — the same function over the same string
that cpmdroid, ioscpm and z80cpmw use, so the four clients name the same
namespace the same way.

### What a fork has to publish

The URL is used **verbatim**, so it need not be a GitHub release — any HTTPS URL
serving the document will do. What it serves has to satisfy the client:

- the index: `"schema": "romwbw-disks-index"`, `"interface": "v0"`, and a
  non-empty `romwbw_versions[]`;
- each entry: `romwbw_version`, an **absolute** `catalog_url`, and
  `catalog_sha256` / `catalog_size` / `generation`;
- the catalog it names: `"schema": "romwbw-disks-catalog"`, a `base_url`, and
  `roms[]` / `disks[]` entries carrying `filename`, `size` and `sha256`;
- the assets themselves, at `base_url + filename`.

Every hash is checked, and the catalog document is verified against the index's
hash and size **before a byte of it is parsed** — so a fork has to publish real
hashes. Generate them; do not transcribe them. `romwbw_disks`'
`tools/gen_catalog.py` computes every one from the built artifact, and
`tools/verify_catalog.py` re-derives them independently.

One constraint is not negotiable: a fork can only publish RomWBW releases this
binary lists in `ROMWBW_SUPPORTED_RELEASES` — 3.5.1 and 3.6.0 today. Anything
else is refused by name before a byte is downloaded, because bank 0 of an
`emu_*` ROM is this repository's HBIOS proxy and a release whose CBIOS calls
something the dispatcher does not implement would load and then misbehave.
`--allow-untested` gets past that, on both this program and the binary.

## Pristine bytes and working copies

**A guest writes to its disks.** `hbios_dispatch.cc` opens every image `"rw"`.
So the hash-verified download and the file the emulator is handed cannot be the
same file: the first CP/M `SAVE` would break the hash, and the obvious repair —
re-download — would destroy the user's work.

    ~/.cache/romwbw_emu/v0/<release>/assets/    verified, mode 0444, safe to delete
    ~/.local/share/romwbw_emu/disks/<release>/  what the emulator writes to

`romwbw-get run` and `path --work` copy into the second on first use.
`verify` only ever looks at the first. If you attach a disk by hand, use
`path --work`.

A guest cannot damage the cache, and does not need to be stopped from trying.
Cached assets are mode 0444, and `hbios_dispatch.cc` falls back to opening `"r"`
when `"rw"` fails, so a cached image attaches and boots normally. What fails is
the guest's first write. Measured, attaching `hd1k_combo` straight out of the
cache and typing `SAVE 1 ZZTEST.COM` at the `A>` prompt:

    [HBIOS DIOWRITE] HD0 short write at LBA 2091 (0/512 bytes) - host disk full or I/O error
    Bdos Err On A: Bad Sector

CP/M retried twice and got the same answer both times. `ZZTEST.COM` was never
created; the image's SHA-256 and mtime were unchanged afterwards, and
`romwbw-get verify` still reported every cached asset ok and 0 bad. That is what
`path --work` is for. Not to protect the cache, which protects itself, but so
that the guest's writes land somewhere instead of coming back as a bad sector.

## When something does not verify

A cached file that no longer matches is one of two different things, and
`romwbw-get` does not guess. It asks the question **per asset**, against the
SHA-256 the catalog published for that one file at the moment it was
downloaded — kept in `<cache>/v0/<release>/fetched.json`:

- **the catalog's hash for it changed** — romwbw_disks re-cut that artifact.
  Expected. The old file is moved aside into `.superseded/` and the new one
  downloaded.
- **the catalog's hash for it is what it always was** — the bytes changed under
  a catalog that did not. That is local damage or tampering, and it is reported
  rather than silently overwritten. `romwbw-get verify --repair` is the
  explicit fix.

This used to be asked per *release*, against the index's `generation` counter,
and that was wrong whenever one re-cut changed **more than one** artifact —
which is the ordinary shape of a re-cut, and one romwbw_disks has already
published ("HB_BNKCALL works, which rebuilds every ROM and bumps both
generations"). The counter was stamped as soon as the first artifact was
repaired, so every other artifact of the same re-cut then found a generation
that had not moved and was reported to the user as local damage: `fetch @rom
@disk0` repaired the ROM and then refused the disk, exit 1, telling them to
inspect a file that was perfectly good. A cache written by an older client has
no per-asset record; those fall back to the `generation` counter until each
asset is next read, which records one.

Nothing is ever deleted on a re-cut. Two of the three GUI clients took the same
position, and named the third's deletion behaviour as a bug they were not going
to acquire.

`verify` walks the assets directory rather than the catalog's `roms[]` and
`disks[]`, so it also re-hashes the upstream `RomWBW-v<ver>-Package.zip` that
fetching `@upstream` puts there, against the catalog's `upstream.package_sha256` -
keying only on the two arrays left a file this program had downloaded, and
could check, reported as "not in the catalog" and left out of the count.
`.superseded/` and `.partial/` are directories and are skipped. The two shell
verifiers prune those same two names when they scan the cache, so a superseded
artifact is kept without being read as a mismatch.

## Exit codes

| | |
|---|---|
| `0` | did the thing, and verified it |
| `1` | a contradiction — a hash, a size, a schema, an id that does not exist, **or a 404 on a URL some already-verified document promised** |
| `2` | could not verify — DNS, a timeout, a 5xx, a rate limit, or a 404 at the index itself. Nothing is known to be wrong |
| `64` | usage |

The 1/2 split is the point. CI turns a `2` into a warning and a `1` into a red
build, because a GitHub outage is not a defect in this repository, and a gate
that cries wolf is a gate that stops being read.

**A 404 lands on either side of that line depending on who promised the URL.**
The index names each `catalog_url` beside its `catalog_sha256` and
`catalog_size`; a catalog names every asset beside its `sha256` and `size`. A
404 on one of those is not "GitHub could not be reached" — it is a document
that has already been verified disagreeing with the release it points at, which
is a broken publish, and it is red. It used to be amber, so a catalog listing a
file the release did not have came out as `could not verify` and the job stayed
green.

The **index's own** URL is the exception, and stays amber: nothing promises it —
HTTPS is the whole of its integrity — so a 404 there is indistinguishable from
an outage, and it is what a `gh release create` without `--latest=false` looks
like. Keeping it amber is also what lets `_index_bytes` fall back to the cached
index and ride the mistake out; a warm-cache user never notices.

`64` exists because of that split. argparse exits 2 on a usage error of its
own, which here is the code that means "could not verify", so `romwbw-get`
overrides it: measured, `romwbw-get --nope`, `romwbw-get frobnicate` and
`romwbw-get path` with no id all exit 64. Left alone, a mistyped flag in the CI
step would have been reported as an unreachable GitHub and the job would have
stayed green.

## The web page

A browser **cannot** fetch these assets from GitHub. Measured: release download
URLs send no `access-control-allow-origin` on either redirect hop and the CORS
preflight returns 404. `raw.githubusercontent.com` and jsdelivr do send `*`, and
carry a byte-identical copy of the catalog *documents* — but the ROMs and images
are release assets only, never committed, so those hosts 404 on them. There is
no Pages site.

So the page reads a manifest and files from its own origin:

    romwbw-get mirror ~/www/romwbw

writes `catalog/manifest.json` and the assets under `catalog/v0/<release>/`. The
page builds its RomWBW, ROM and disk lists from that manifest at load time, and
checks each download against the size and SHA-256 the manifest carries (via
`crypto.subtle`, which needs a secure context — over plain HTTP the page says it
checked the size only rather than reporting a pass it did not perform).

`make -C web deploy-dev` and `deploy-romwbw-PRODUCTION-ASK-HUMAN-FIRST` run the
mirror as part of the deploy, so **a newly published disk reaches the page by
re-running a deploy** — no source edit, no wasm rebuild, no release.

An installed `.deb` has no mirror. The page says so, in its status line and in
the terminal, and the file pickers still work. `romwbw-get mirror
/usr/share/romwbw_emu/web` populates it.

## Reproducing the ROM from source

Bank 0 of every `emu_*` ROM is this repository's `src/emu_hbios.asm`; banks 1–15
are an upstream RomWBW ROM, unmodified. `roms/build_emu_rom.sh` assembles bank 0,
overlays it, and — this is the point — compares the result against the SHA-256
the catalog publishes:

    $ roms/build_emu_rom.sh
    ...
    PASS: byte-identical to the published emu_avw for RomWBW 3.5.1
          sha256 4b11402a29fad22de304775b7c415eb6a74600df06bd57828b9931a7e9693258

It builds only the release `ROMWBW_DEFAULT_*` in `src/romwbw_pin.h` names, because
`src/emu_hbios.asm` here hardcodes its version stamp. The copy in romwbw_disks
takes that stamp from a generated include and can build any release;
`romwbw_disks/tools/build_rom.sh` is what cuts what users download. Overlaying
this bank 0 on another release's banks would produce a ROM that boots and then
prints `*** WARNING: HBIOS/CBIOS Version Mismatch ***`, so the script refuses it.

## What is checked, and where

| Check | Needs a network? | Runs in |
|---|---|---|
| `tests/catalog_client_test.py` — the client against a catalog it serves itself | no | `make -C src test` |
| `tests/web_manifest.js` — the page's manifest → `<option>` logic | no | `make -C src test` |
| `disks/verify_disk_utils.sh` — `src/{r8,w8}.asm` build, and w8 keeps its `HBF_HOST_CAPS` interlock | no | `make -C src test` |
| the same script against the images in the cache | yes | CI, after a fetch |
| `roms/verify_romwbw_pin.sh` — the binary's release list vs `src/romwbw_pin.h` | no | `make -C src test` |
| booting the published ROM and disk to a CP/M prompt | yes | CI, after a fetch |

Both shell verifiers read the `romwbw-get` cache - `ROMWBW_GET_CACHE` if it is
set, else `$XDG_CACHE_HOME/romwbw_emu`, else `~/.cache/romwbw_emu` - so what
they inspect is the bytes that were actually downloaded rather than what the
catalog says about them. `disks/verify_disk_utils.sh [image-dir ...]` takes
directories that replace that scan; `roms/verify_romwbw_pin.sh [tree_root]`
takes a tree to check *as well as* the cache.

The network ones are tri-state (see the exit codes above): unreachable is a
warning, contradicted is red.
