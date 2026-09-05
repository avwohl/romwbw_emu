# Changelog

All notable changes to **romwbw_emu** are documented here.

This file starts at `v1.35`. For most of this project's life the record of a
change has been the commit message that made it, and those messages are longer
and more specific than any changelog entry — they carry the measurements, the
counter-examples and the things that were deliberately *not* done. This file
summarises and points; `git log` is the detail. Open work some machine could
take is in [`todo.txt`](todo.txt), and the open questions waiting on the owner's
ruling are in [`DECISIONS.md`](DECISIONS.md); what downstream ports have to do
about a release is in [`DOWNSTREAM.md`](DOWNSTREAM.md) and
`docs/DOWNSTREAM_*.md`.

Downstream ports do not consume a *release* of this repository — `ioscpm`
symlinks into `src/`, `z80cpmw`'s vcxproj compiles it in place, and `cpmdroid`'s
CMakeLists pulls it from a sibling checkout — so a commit here reaches all three
on their next build, tag or no tag.

## [Unreleased]

`VERSION` is `1.39`. The rule `[1.37]` was cut to establish is that a
**binary-affecting** change landing here bumps it, so that no build answers with
a tag's version while differing from it. The RomWBW runtime-version change below
touches four files in `src/`, so it bumped. Cutting a release from here takes a
dated heading, a `VERSION` that already matches the tag, and a green
`workflow_dispatch` run of `release.yml` before the tag exists; that dry run is
what replaced the dependency pins, so it is a step rather than a courtesy.

### Added

### Fixed

- **`src/makefile` had no header dependencies at all.** The pattern rule was
  `%.o: %.cc` and nothing else, so editing any header rebuilt nothing: `make`
  reported "up to date" and the binary kept whatever was compiled in before.
  Found by editing `romwbw_pin.h` - the file whose entire job is to be the
  single source of truth about the RomWBW version - and watching `--version`
  keep answering with the old one. `-MMD -MP` plus `-include $(wildcard *.d)`
  now records and replays them; `clean` and `.gitignore` take `*.d`.

- **`roms/verify_romwbw_pin.sh` was never run by anything.** Not by `make test`,
  not by any CI job - only by a human remembering its name. `make -C src test`
  runs it now, which is how a stale binary or a mismatched artifact gets caught
  without anyone remembering.

### Changed

- **`roms/verify_romwbw_pin.sh` asks a different question.** It used to be "does
  everything in this tree match the one pinned release?", which would now fail a
  tree that correctly ships both. It is now "is every artifact a release this
  core can run, and does every disk have a ROM to pair with?" - plus a new check
  that no single image carries slices from two releases, and a binary check that
  compares `--version`'s list against the header rather than one string. The
  file name is kept because DOWNSTREAM.md, README.md and three client CHANGELOGs
  already name it; there is no pin left for it to verify.

- **`ROMWBW_PIN_*` is now `ROMWBW_DEFAULT_*`**, and means something narrower:
  the release *this tree's own* `roms/` and `disks/` artifacts are cut from, for
  the build scripts to read. It is not a constraint on what the binary can load.
  Renamed rather than repurposed so that anything still reading the old name
  fails loudly. `roms/build_emu_rom.sh` and `roms/build_from_source.sh` follow;
  `roms/emu_avw.rom` rebuilds byte-identical
  (`c7abc580b3285a33e439c0d6724a9d64dd3e93733a4fc2c1b80b0bfd91f9c580`).

- **`disks/disks.xml` stopped calling CP/M Plus broken.** The entry read
  `CP/M Plus (CP/M 3.0) - NOT WORKING, under investigation.` It boots: booting
  slice 3 of `hd1k_combo.img` headless prints `CP/M v3.0 [BANKED] for HBIOS
  v3.5.1` and `60K TPA`, and `DIR *.COM` lists the CP/M 3 utility set
  (`GENCPM`, `SETDEF`, `DEVICE`, `HEXCOM`, `INITDIR`, `PATCH`, `PUT`/`GET`,
  `ZSID`). The corrected wording is the one `ioscpm` already carries at
  `release_assets/disks.xml:39`, written for `v1.4.1` ("Update CP/M 3
  description - now working with bank config fixes") and never propagated back
  here. Documentation only; no asset changed and the binary is untouched. One
  caveat worth knowing before automating against it, and not recorded anywhere
  else: CP/M 3's `DIR` paginates and then blocks on `Press RETURN to Continue`,
  where CP/M 2.2's on the same disk runs to completion — a piped capture
  truncates there and then hangs.

- **Step 4 of the release-order plan is done: the refreshed disk images are
  published as `ioscpm` `v1.4.12`.** A prerelease, 29 assets, 2026-09-01. Only
  `hd1k_combo.img` needed refreshing - all 21 published `v1.4.5` assets were
  opened and the other 19 carry no `R8`/`W8` at all - so they went up
  byte-identical and the catalog diff is one line, the combo's `<sha256>`
  moving to `89b8ae1a...`. The image was rebuilt from the *published* bytes
  rather than from `disks/hd1k_combo.img` here, because the tracked image is a
  superset carrying seven developer scratch files; the result is deterministic
  at 51380224 bytes and `disks/verify_disk_utils.sh` passed on the exact
  uploaded bytes, interlock assertion included. `disks.xml`'s `version`
  attribute was deliberately left at `13`, so the wipe that deletes every `.img`
  in a user's `Documents/Disks` never fired. Eight post-publish gates held:
  `releases/latest` is still `v1.4.11`, `v1.4.5` is still `be19984e...` with 30
  assets, and `releases/latest/download/disks.xml` still hashes `6ae94b8c...`.
  **No port pin was bumped** - that is step 5, and it stays blocked.
  `hd1k_combo_ioscpm_w8fixed.img` was dropped and must not be republished: it is
  byte-identical to the *unfixed* combo and its name claims a fix it does not
  carry.

### Docs

- **`docs/RELEASE_ORDER_2026-08-25.md` corrected on the point the whole plan
  rested on.** It said new assets under a new tag "reach nobody until a build
  points at them". That holds only for `ioscpm` builds 42 and later. The App
  Store is on **1.4.9, released 2026-03-19** - measured with
  `itunes.apple.com/lookup` for `com.awohl.cpm` - and `MARKETING_VERSION 1.4.9`
  maps to builds 36/37, which predate the catalog pin and fetch from
  `releases/latest/download/`. For every device in service a *normal* release is
  fetched immediately, and those builds contain the invalidation wipe. What
  actually protected users at step 4 was `--prerelease`, which is now a numbered
  pre-flight gate rather than an implicit habit. Step 4 is marked done with its
  evidence, and a warning records that `ioscpm/docs/DISK_W8FIX_RUNBOOK.md` tells
  the reader to `gh release upload <tag> --clobber` against `v1.4.5` - the one
  action the plan forbids absolutely.

### Removed

### `disks/disks.xml` is deleted: it was an inventory of nothing

`version="6"`, 21 `<disk>` entries, zero `<sha256>` elements - and the numbers are
the argument. Nineteen of the twenty-one filenames it listed name images this
repository does not track, and `hd1k_infocom.img`, one of the two it does track,
it never listed at all. It described neither what is here nor what any client
fetches.

Nothing read it. `roms/verify_romwbw_pin.sh` is the only thing that walks the
tree and its two `find` expressions take `*.rom`/`*.bin` and `*.img` only, so a
`.xml` never enters either scratch list. `disks/verify_disk_utils.sh` and
`disks/rebuild_disk_utils.sh` both hardcode
`disks/hd1k_combo.img:wbw_hd1k_0 disks/hd1k_infocom.img:wbw_hd1k` rather than
globbing the directory. `release.yml`'s staging step never enters `disks/`, so no
`.deb` or `.rpm` carried it. Confirmed after deleting: `make -C src test` passes
with the three counts CI asserts unchanged - 4 disk-resident binaries, 2 shipped
`w8.com` with the interlock, every artifact naming a supported release.

The catalog the three ports actually fetched was always ioscpm's published one,
`version="13"` with a per-disk `sha256`, and `tools/check-disk-pins.sh` reads that
over HTTP. It is untouched by this and was never pointed here.


- **One binary now boots more than one RomWBW release.** The compile-time pin is
  gone. `emu_validate_rom_hcb()` compared a loaded ROM's HCB version bytes
  against `ROMWBW_PIN_VER_BYTE`/`ROMWBW_PIN_UPD_BYTE` and failed the load on any
  difference, so a build pinned to 3.5.1 physically could not load a 3.6.0 ROM -
  which is why every downstream client could *fetch* two RomWBW versions and run
  only one. The version is now read out of the loaded ROM at run time, and this
  build boots v3.5.1 and v3.6.0. `--version` says so:

      RomWBW releases this build can run: 3.5.1, 3.6.0
        (the version a guest sees is read from the ROM it loads, not compiled in)

  Five sites report a version to the guest and every one of them now derives it
  from the ROM rather than holding a copy: `HBF_SYSVER`
  (`hbios_dispatch.cc:1537`), the NVRAM checksum seed (`:697-707`), the HBIOS
  ident block (`emu_init.cc:283`, `:289`), the CBIOS page-zero stamp at
  `0x42`/`0x43` (`:324-325`), and the load-time check itself. **The last two
  exist only in emulated RAM.** Nothing in this tree inspects emulated RAM, so a
  stored copy that was never updated would have been invisible to every verifier
  here and would have surfaced only as a guest printing the wrong version.
  Deriving rather than storing is what makes that unrepresentable: there is no
  cached value and no initialisation order to get wrong, only `read_bank(0,
  0x105)`.

  Verified by running it, not by reading it. A CP/M program assembled for the
  purpose (`R8`-imported, reading `0x42`/`0x43`, `0xFE02`, the block `0xFFFC`
  points at, and `HBF_SYSVER`) reports `3510` under a 3.5.1 ROM and `3600` under
  a 3.6.0 ROM, on all four - which is the only way those two RAM-only sites can
  be checked at all. Both releases boot CP/M 2.2, banked CP/M 3, ZPM3, Z3PLUS,
  ZSDOS and NZCOM; `R8`/`W8` round-trip a file byte-identically under both; the
  boot loader prints `NV Switches Found` under both, so the checksum seed agrees
  with the ROM's own SYSCONF.

- **`ROMWBW_SUPPORTED_RELEASES`, and a refusal that means something.** A release
  this core has never been checked against is still refused, because bank 0 is
  our HBIOS proxy and a release whose CBIOS calls a function the dispatcher does
  not implement would load and then hang - much worse than a refusal. The
  message names the release and what this build can run:

      ROM is built for RomWBW v3.7.0, which this emulator has not been checked
      against (it can run 3.5.1, 3.6.0) - use one of those, or add v3.7.0 to
      ROMWBW_SUPPORTED_RELEASES in src/romwbw_pin.h once you have booted it

  `emu_set_allow_untested_romwbw(true)`, or `--allow-untested-romwbw`, overrides
  it with a warning. Adding a release is a claim that somebody ran it, and each
  `X()` entry carries the date it was checked.

- **New public API in `emu_init.h`** for ports that show a version:
  `emu_romwbw_release_of_image()` (inspect an image before offering it in a
  picker), `emu_romwbw_release_loaded()`, `emu_romwbw_release_str()`,
  `emu_romwbw_release_supported()` and `emu_romwbw_supported_list()`.

## [1.38] - 2026-09-01

This release is nine commits of build and bookkeeping work on top of `v1.37`,
cut the same day. **It changes no emulator behaviour**, and that is measurable
rather than asserted: `git diff v1.37..HEAD -- src/` touches one file,
`src/makefile`, and only a comment in it - not one `.cc` or `.h` changed, so the
binary differs from `v1.37`'s in the version string and nothing else. A package
user has no reason to upgrade for the emulator; the reason to cut it is that
`VERSION` must not keep answering `1.37` for a tree that is not `v1.37`.

What did change is how a release is made and what the repository says about
itself: the workflows, `CHANGELOG.md`, the new `DECISIONS.md`, `README.md`,
`todo.txt` and `web/README.md`.

The headline is that **CI pins nothing now** - see **Changed** - which makes a
`workflow_dispatch` dry run of `release.yml` a required step before any future
tag rather than an optional check. That dry run was run for this one, green on
both arches, before the tag existed.

### Fixed

- **`release.yml` got its CRLF endings back after a whole-file rewrite.**
  `1e4a07e` changed twenty lines and produced a diff of 255 insertions and 234
  deletions, and the diff size is the tell. The cause was editing the file with
  Python in text mode, which reads CRLF as LF and does not translate back on
  Linux, so every line ending in the file was silently rewritten around a small
  edit: `release.yml` went from 234 CRLF lines out of 234 to 0 out of 255, and
  is all-CRLF again now. Silent, whole-file, and no test will ever fail on it.

  `git diff --ignore-cr-at-eol 41565a1 2dd99df` is what proves nothing else
  moved: 24 insertions and 4 deletions in `release.yml` alone, which is the
  twenty-line change plus one comment paragraph rewrapped because the broken
  edit had left it ragged. Write it with **both** revisions - the one-rev form
  diffs against the working tree, so as a durable instruction it would prove
  the opposite the moment the tree moved on.

  This is the **second** time this failure mode has hit this repository, which
  is the reason to write it down again. The first is recorded in `[1.37]`
  below, against `src/emu_io.h` inside the `v1.36` tag, and that record is how
  this one was recognised. `test.yml` escaped in the same session for a reason
  rather than by luck: it was edited with `perl -pi`, which does not translate
  endings, and stayed at 270 CRLF lines out of 270 at `41565a1`, `1e4a07e` and
  `2dd99df` alike.

- **One arch failing no longer cancels the other in `release.yml`.** GitHub's
  matrix default is `fail-fast: true`, and the two build jobs are independent -
  each compiles, packages and uploads its own four assets, with no step reading
  the other's output - so the default bought nothing and had already cost a
  release. Run `31176917009`, 2026-08-07: the `ubuntu-24.04-arm` job failed, the
  `ubuntu-latest` job was **cancelled** mid-build, `collect` was skipped, and the
  release came out with no assets on either arch rather than the four that were
  about to succeed on amd64. Recovering it took a force-moved tag - that run's
  headSha is `6f4bc554`, the successful one's is `13d1c239`, and
  `git rev-parse v1.35^{}` is `13d1c239` today - which is exactly the move
  `docs/RELEASE_ORDER_2026-08-25.md` tells every port never to make on a
  published tag. With `fail-fast: false` a half-failed release keeps the good
  arch's assets and `gh run rerun --failed` completes the set with the tag
  untouched. `collect` is still skipped when an arch fails, which is correct: it
  only lists what was built. `test.yml` needs no equivalent change - its three
  platforms are separate jobs, not a matrix.

### Changed

- **The four dependency pins are gone; a dry run of `release.yml` before
  tagging replaces them.** `EMSDK_VERSION`, `UM80_VERSION`, `CPMEMU_REF` (all
  four value sites) and the `FPM_VERSION` added and removed the same morning,
  two commits apart, are gone, so fpm, emscripten, um80 and the cpmemu clone
  that supplies the Z80 core now track whatever those projects ship. Owner's
  call, made after the alternative had been run here rather than argued in the
  abstract, and the case for pinning is not being written out of the record: it
  is real, and it has cost this repository twice. `13d1c23`, where `emcc`
  stopped implying the C++ driver at link time and the web build failed on a
  commit that had not touched it; and the cpmemu makefile change that put a
  Clang-only `-W` flag in unconditionally and stopped `make libqkz80.a` on
  every GCC runner, which is
  the `CPMEMU_REF` bump `[1.37]` below records as a bug fix rather than a
  routine move. What was wrong with pinning was the price. Pins rot silently -
  they stop collecting fixes, the bump becomes a cliff, and in a one-maintainer
  project only one person can ever do it - and here they promise more than they
  deliver, because `ubuntu-latest`, every apt package and the emsdk tooling
  repo float regardless: a pinned fpm under a moving OS is not a reproducible
  build, only a partial one that reads like a whole one.

  What replaces them is the thing that would have caught both of those failures
  anyway - running the release workflow before it matters. `release.yml` is
  `workflow_dispatch`-able, and both the upload step and the `collect` job are
  gated on the release event, so
  `gh workflow run release.yml --ref main -f version=X.Y` builds everything and
  publishes nothing. An "On pinning" header at the top of `release.yml` now
  carries that command and the whole argument, so the next person to reach for
  a pin finds the reasoning instead of a vacuum; it also records the hyphen
  trap that cost a red dry run, since rpm rewrites `-` in a version and the
  version-less copy step matches fpm's filename literally.

  Two things make the floating cpmemu clone tolerable, and both are written
  where they apply. `test.yml` clones the same unpinned default branch on every
  push here, so a cpmemu change that breaks this repository surfaces on the
  next commit rather than at tag time - that is what the floating core is
  bought with. And only the `qkz80*` sources reach this binary at all, so
  `git diff <a>..<b> -- 'src/qkz80*'` is the check for whether a cpmemu move
  touched the core; across the last two refs this repository pinned it returned
  `qkz80.pc.in` alone, with `libqkz80.a` byte-identical, measured in `[1.37]`
  below.

  um80 keeps the sharpest edge of the four, and `test.yml` says so where it
  installs it: `disks/verify_disk_utils.sh` compares the `r8.com`/`w8.com` on
  each image byte for byte against a fresh assembly, so a um80 release that
  changes its output turns that job red with nothing in this repository
  touched. That is the intended signal rather than a defect - it means the
  committed images no longer match their sources - and the answer is
  `disks/rebuild_disk_utils.sh`, not a pin.

  Two pins are deliberately left, and are a different class: `setup-python`
  still takes `python-version: '3.12'` in `test.yml`, and every action is at a
  major tag (`actions/checkout@v6` and friends, plus the third-party
  `softprops/action-gh-release@v3` that holds `contents: write`). Those are the
  runner's own plumbing and a supply-chain question respectively, not the
  build-tool versions this entry is about.

  Stale references outside the workflows went with the pins. `src/makefile`
  said this tree "goes quiet when `CPMEMU_REF` moves past it"; `todo.txt` said
  `CPMEMU_REF` "has to move past it in both workflows and in release.yml".
  There is no `CPMEMU_REF` to move, so both now say it goes quiet on the first
  build after cpmemu ships the fix, with nothing to bump here.

  Verified rather than assumed, at `d18e374` itself: the dispatch dry run
  `33531118963` is green on both `ubuntu-latest` and `ubuntu-24.04-arm` with
  `collect` skipped - which is the release-event gating doing exactly what the
  header claims - and `test.yml` run `33531083094` is green on all three of
  `test`, `macos` and `msvc`.

- **`release.yml` no longer installs um80**, or the `python3-pip` that was
  there to install it. um80 was installed "for 8080/Z80" and never invoked:
  that workflow assembles nothing, runs no tests, and builds `r8.com`/`w8.com`
  from no target - nothing does, which is why `disks/verify_disk_utils.sh`
  exists to check the copies on the images against their sources, under
  `make -C src test`, which only `test.yml` runs. So the release job was
  carrying an unpinned network install of a tool it never uses: a dependency
  that could only ever break a release, never help one. emsdk needs `python3`,
  which the runner has, not pip. This removal stands - the same commit also
  pinned fpm, and it is only that pin the unpinning above took back out.

### Docs

- **The six `[DECISION]` items moved out of `todo.txt` into `DECISIONS.md`.**
  `todo.txt`'s header makes two promises - that checks needing a person at a
  keyboard are in `MANUAL_CHECKS.md`, and that "every item carries a tag saying
  what a machine has to be able to do to take it, so a session on another OS can
  see at a glance what it can pick up" - and `[DECISION]` honoured neither. It
  names no capability; it says the opposite, that *no* machine can take the item
  at any time, because what blocks it is the owner's judgment rather than a
  tool, an OS or a credential. The cost of leaving them there is measured rather
  than argued: the audit recorded below re-verified all six against `HEAD` on
  2026-09-01, found every fact they rest on still standing, and moved none - and
  that is what each future audit would also do, because evidence was never what
  they were short of. `DECISIONS.md` is a sibling of `MANUAL_CHECKS.md` on the
  principle that file already established: work needing a human comes out of the
  machine's list, whether the human runs a check or makes a ruling. Everything
  measured moved with them - the `826c3bcf7db18a36f8eb73792873613d` md5 the four
  ROM files share, `STAT DEV:` and `PIP LST:=SOURCE.TXT` under CP/M 2.2 on
  `hd1k_combo.img`, `SYSGET_CIOCNT` answering 1, the seven uncalled aux/printer
  functions by name, the `"100%\rDONE"` and `0xC3 0xA9` console cases and the
  WordStar reason behind the `0x7F` mask, `emu_host_file_open_write()`'s
  unconditional `true` against the `HBF_HOST_*` codes four ports implement, and
  the `boot`-key write-back - nothing was summarised away. `todo.txt` is 13
  items down to 7, every one of them takeable by some machine. Entries in
  `[1.37]` and `[1.36]` below that send a reader to `todo.txt` for one of these
  questions - the packaged `.img`, the callerless aux/printer family, the
  `0x7F` mask - were true when written and are left exactly as they are; this
  repository does not rewrite shipped history, and the new pointer in
  `todo.txt`'s own header is what catches a reader who follows one.

- **`todo.txt` re-checked item by item against `HEAD`, and nothing closed.** All
  13 items were verified against the code rather than against their own text:
  none is finished, so none moved here. Six are `[DECISION]` items whose whole
  content is a question only the owner can settle, and the facts each rests on
  were re-measured and still hold; those six have since moved to
  [`DECISIONS.md`](DECISIONS.md), above. Three carried text that had gone stale
  and were corrected in place rather than deleted:
  - the first `[BROWSER]` item said the checks need "a wasm built elsewhere:
    emcc is not on this machine". That stopped being true when CI run
    `33519212283` built one from `c750678`: the v1.37 `.deb` ships
    `romwbw.html`, `romwbw.js`, `romwbw.wasm` and `vendor/` as a servable
    layout, and the packaged page is byte for byte this tree's template
    rendered. The item now says so. It also carried a trap that has since been
    closed: `web/` held gitignored March leftovers of `romwbw.js`
    (md5 `40c9e15e...`) and `romwbw.wasm` (`18ef70a2...`), so serving that
    directory would have answered all three checks against a five-month-old
    wasm. Both are deleted with `make -C web clean`, which is the makefile's own
    target for them, so `make serve` and the two deploy targets now fail on the
    absent `emcc` rather than quietly shipping a stale build. The December 2025
    `romwbw_node.*` and `romwbw_test.*` pairs, which `clean` does not name, went
    with them, so no `.wasm` remains in `web/` - see below. What is left is a
    person at a browser.
  - the `[RELEASE]` item said steps 0 and 1 of the release-order document are
    cleared; `c750678` moved steps 2 and 3 to "Code done" as well, so step 4 is
    the next one. It also conflated two `disks.xml` files: this tree's is
    `version="6"` with 21 entries and no checksums, while the catalog the three
    ports actually fetch is `ioscpm`'s, `version="13"` with 20 entries **and** a
    per-disk `sha256` - and it is that file's version attribute whose bump fires
    the disk-wipe hazard. The published `hd1k_combo.img` was dissected rather
    than assumed: its `w8.com` is 1280 bytes, rebuilds byte for byte from
    `w8.asm` at `3101b6d`, still has `cp 1Ah` in the copy loop and no
    `06 e9 cf` interlock probe. The other 19 published images were not opened.
  - the `[WINDOWS]` item credited `z80cpmw`'s CI with executing Windows code.
    That port has no CI at all (`gh api repos/avwohl/z80cpmw/actions/workflows`
    answers 0), and cpmemu has no test workflow either. Of cpmemu's two Windows
    test sections only "windows console" needs a real machine; "windows
    cross-compile" skips for want of `x86_64-w64-mingw32-g++`, which is
    installed here, and is another compile rather than a run.

- **The unrebuildable Node wasm builds in `web/` are deleted too.**
  `romwbw_node.js`/`.wasm` and `romwbw_test.js`/`.wasm` were December 2025
  Emscripten builds, never tracked, and `web/README.md` recorded that "the
  makefile has no target to rebuild them" - so they had drifted five months
  behind the sources with no way to notice and no way to refresh. Their only
  consumer was `web/test_roms.js`, itself a gitignored leftover; the two web
  tests `make -C src test` actually runs (`tests/web_console_output.js`,
  `tests/web_reload_disks.js`) read `romwbw.html-template` and never touched a
  wasm, so the suite is unaffected. Deletion is permanent by construction - a
  file no target builds and no commit holds cannot come back - which is the
  argument for removing it rather than leaving it to be trusted by accident.
  `web/README.md` now says they are gone and that `test_roms.js` cannot run
  until someone writes a Node-targeted build target.

## [1.37] - 2026-09-01

`VERSION` says `1.37` and the tag is `v1.37`. It does **not** follow a released
`v1.36`. `v1.36` was tagged at `04ad2b6` on 2026-08-25 and pushed
(`refs/tags/v1.36` peels to `04ad2b6` on `origin`), and then no release workflow
ever ran for it: the newest run of "Build and Release Packages" is
`31186495366`, a `workflow_dispatch` on 2026-08-07, and `gh release list` still
shows `v1.35` as `Latest`. So no deb, no rpm and no CI-built wasm was ever
produced for `v1.36`, and **`v1.37` is the first packaged release since `v1.35`**
(2026-08-07). A package user goes 1.35 -> 1.37 and takes 29 commits of change at
once: the 23 inside the `v1.36` tag, written up in `[1.36]` below, and the 6
after it (`04ad2b6..8eeb227`), written up here. `git log v1.36..v1.37` shows one
more than that — the release commit itself, which sets `VERSION`, moves
`CPMEMU_REF` and writes this entry.

The `v1.36` tag itself stands untouched — not retracted, not moved, not re-cut.
It is the published core-ABI contract three downstream ports coordinate against:
`emu_host_path_caps()` is undefined in the core from `04ad2b6` onward, and
[docs/DOWNSTREAM_2026-08-25.md](docs/DOWNSTREAM_2026-08-25.md) names that tag as
where it starts. Moving it would take away the one fixed reference those ports
have. A tag that was never packaged is a different thing from a release that
shipped, and nothing below should be read as saying `v1.36` shipped.

**One build-contract change, so read this before syncing.** `emu_io.h` declares
`emu_host_file_get_read_name()` and the core does not define it, exactly as it
does not define `emu_host_path_caps()` — a port that takes these sources without
adding it fails to *link*. `return "";` is a correct answer and costs nothing:
`HBF_HOST_GETRNAME` then reports "no answer" and `R8` prints what was typed,
which is what it printed before. See
[docs/DOWNSTREAM_2026-08-26.md](docs/DOWNSTREAM_2026-08-26.md).

Both workflows now pin `CPMEMU_REF: 9a94e8d` instead of `9fee3c2`. That is a
bug fix, not a routine bump: `9fee3c2` put the Clang-only
`-Wshorten-64-to-32` into cpmemu's `src/makefile` unconditionally, and GCC
fails rather than ignores an unknown `-W` flag, so `make libqkz80.a` in the
"Build qkz80 library" step could not succeed on a GitHub runner. `test.yml`
has been red at that step since the pin was set; cpmemu `9a94e8d` probes for
the flag instead. Verified by replicating both steps here against a clean
clone at `9a94e8d`: `make libqkz80.a` with no diagnostics, then the suite with
the same explicit `QKZ80_CFLAGS`/`QKZ80_LIBS` the workflow passes, 65 and 53
checks, `ALL TESTS PASSED`. The bump also takes cpmemu's 8080 `DAA`/`CMA`/
`STC`/`CMC` flag fixes; all four are gated on `MODE_8080` and this emulator
runs Z80, and cpmemu's zexdoc and zexall each still complete 67 groups with no
CRC mismatches.

`CPMEMU_REF` then moved once more for this release, `9a94e8d` -> `91151f1`, at
all four value sites (`test.yml:97`, `:185`, `:251`, `release.yml:89`). **That
one is provenance, not function.** `91151f1` is cpmemu's `v4.7.2` tag, so the
pin now names a tagged cpmemu release instead of a mid-stream commit; the core
it produces is unchanged. Measured rather than asserted: `libqkz80.a` built from
a clean tree at each ref, same compiler, same flags, is byte-identical — 118976
bytes, md5 `374bde15a856cef17b89388c6576ff2f`, both times. The eight commits in
between touch six files under `src/` — `cpmemu.cc`, `os/linux/platform.cc`,
`os/windows/platform.cc`, `CMakeLists.txt`, `makefile`'s dylib branch and
`qkz80.pc.in` — and this repository compiles none of them: it links the archive
and includes the `qkz80_*.h` headers, and
`git diff --name-only 9a94e8d..91151f1 -- 'src/qkz80*'` returns `qkz80.pc.in`
alone. So cpmemu `v4.7.2`'s BIOS `SECTRAN`, its `CPM_BIOS_DISK` handling,
`--save-memory` fix and its Windows console rewrite are all real and all in
`cpmemu`, and **none of them is in this binary**. This release must not be
credited with any of them. Nor with the 8080 `DAA`/`CMA`/`STC`/`CMC` flag
fixes, for a different reason: those landed *in* `9a94e8d` itself
(`git log -S'MODE_8080' -- src/qkz80.cc`), so they arrived at the previous
bump, in `657d61a` — and they are gated on `MODE_8080`, which this Z80
emulator never sets, so they were inert then too.

It does not close the `qkz80_MK_INT16` narrowing item in `todo.txt` either,
which is the one thing a cpmemu bump could plausibly have closed.
`qkz80_types.h` and `qkz80_reg_pair.h` are byte-identical at both refs, and
`clang++ -Wall -Wimplicit-int-conversion -Wshorten-64-to-32 -fsyntax-only
-Isrc -I<cpmemu>/src src/hbios_dispatch.cc` — the `-I` at each ref's headers is
what "against each of them" means, and without it the command stops at
`'qkz80.h' file not found` — emits the same two warnings both times, at
`qkz80_reg_pair.h:32` and `:35`. `src/makefile`'s comment — "this repo goes
quiet when `CPMEMU_REF` moves past it" — is still true and still unredeemed, and
is deliberately left as written: the fix it points at is in cpmemu's future, not
in `v4.7.2`.

Two version numbers have to be kept equal by hand, which is the other reason
this section is `[1.37]` and not `[1.36]`. The CLI binary's `--version` string
and the wasm's compiled-in one come from the `VERSION` file (`src/makefile:7`
and `web/makefile:18`, both feeding `-DEMU_VERSION`). The deb and rpm filenames
and package metadata, and the `@VERSION@` the rendered page prints in its banner,
come from the **tag**: `release.yml:94-96` reads
`github.event.release.tag_name` and strips the leading `v`, and that value is
what reaches `:119`, `:156`, `:169` and `:181-182`. Nothing compares the two. A
tag that disagrees with `VERSION` builds a `romwbw-emu_1.37_amd64.deb` whose
binary answers `v1.36` to `--version`, and a page whose banner disagrees with
the version compiled into the wasm beneath it, and no step goes red anywhere.
Hence: `VERSION` is `1.37`, the tag is `v1.37`.

The disk images in `disks/` changed: they carry the new `r8.com` and `w8.com`
and nothing else. Both were rebuilt with `disks/rebuild_disk_utils.sh` from the
committed images, so `cpmls` of each one lists exactly what its committed
version lists — an earlier working copy of `hd1k_combo.img` had picked up two
files from guest-side testing (`rd.$$$`, `source.txt`) and that is not in this
diff. Everything else here is the CLI, the build and the web page.

**The images are not part of this release.** `release.yml`'s "Upload to Release"
step attaches `*.deb` and `*.rpm` and nothing else (`release.yml:200-202`), and
its staging step copies the binary, the rendered page, the wasm, `web/vendor/`
and `roms/*.rom` — no `.img` at any point (`release.yml:122-150`). So the
refreshed `r8.com`/`w8.com` above reach **nobody** through this channel; a
reader of the paragraph before this one would otherwise assume they do. They
travel through `ioscpm`'s release assets, which all three port catalogs pin,
under the ordering constraint in
[docs/DOWNSTREAM_2026-08-25.md](docs/DOWNSTREAM_2026-08-25.md): the sanitiser
fix has to land in `ioscpm` before, or with, the new images. That separation is
the same one `[1.36]` records and it has not changed. The consequence for a deb
user is still open in `todo.txt` as a policy question: nothing packages an
image, so a stock install 404s on every `.img` name the page's select offers.

### Added

- **`R8` says which file it actually opened** — new HBIOS extension function
  `HBF_HOST_GETRNAME` (0xEA), the read mirror of `HBF_HOST_GETNAME` (0xE8) down
  to the calling convention: `C` = buffer size at `DE`, `A` = 0 and the buffer
  holds the effective source, `A` = 0xFF and the buffer is untouched. The
  "Reading:" line printed the path the CCP shouted, and it moved below the open
  for the same reason `W8`'s "To host:" did — before the open there is nothing
  to ask about. Run on Linux against a directory called `MixedCase`,
  `R8 <dir>/MIXEDCASE/SOURCE.TXT` now prints
  `Reading: <dir>/MixedCase/source.txt` — the file the emulator really opened,
  in the case it really has, absolute. That path is what the CLI backend
  resolved on its case-insensitive retry; the old line echoed the shouted string
  and named nothing on a case-sensitive filesystem. One consequence smaller than
  the write side, which is why it came later: a read creates nothing the user
  then has to find.

  The browser deliberately answers nothing. Its read is a file *picker*, so the
  guest's string is a hint the user is free to ignore, and echoing it would be
  wrong rather than merely unhelpful; the picked file's real name never reaches
  the core. `emu_io_wasm.cc` returns `""` and says so.

  Both getters now share one bounded copy (`HBIOSDispatch::storeHostName`), so
  the ".../tail" cut rule cannot drift between them.
- **`--boot=none`** (and `off`, the spelling `--escape` already uses) — the only
  way back to the boot menu once a target is persisted. Setting the in-core
  NVRAM to "uninitialized" is not enough: the setting lives in a file that
  outlives the run, so this removes it.

  It removes exactly one file — the `nvram` under the config directory *this
  run* selected — and it does **not** remove the pre-XDG file in `~/.config`.
  That one is still *read* as a migration fallback, so a setting there does come
  back on the next run; `--boot=none` names the file and says to remove it by
  hand. The first attempt did delete it, guarded on the condition the loader
  reads it on — the current path yielding nothing — and that guard was worthless,
  because the current path yielding nothing is exactly the state the *first*
  `--boot=none` leaves behind. So a second `--boot=none` under
  `XDG_CONFIG_HOME=<temp dir>` reached outside that directory and deleted the
  developer's real `~/.config/romwbw_emu/nvram`. That was found by running it,
  on the real file, twice. The current build leaves it alone — three
  `--boot=none` runs under a temp config home and `~/.config/romwbw_emu/nvram`
  still holds what it held — and the second of those runs says `nothing to clear
  at <path>` rather than claiming to have cleared a target it never found, which
  is what the deletion made it say. Nothing here can tell a user who has
  genuinely relocated their config from a script pointing `XDG_CONFIG_HOME` at a
  temp directory, which is why it reports rather than guesses.
- **`disks/diskdefs`** and both disk scripts running `cpmtools` from that
  directory. Debian and Ubuntu's `cpmtools` 2.23 ships `wbw_hd1k` and nothing
  per-slice, so on those machines `verify_disk_utils.sh` reported
  `hd1k_combo.img r8.com is not on the image at all` for a file that is on it,
  and `make -C src test` failed — which is the exact package
  `.github/workflows/test.yml` installs. `cpmtools` reads `./diskdefs` **or** the
  system file and never both, so the local one has to be complete rather than a
  supplement. It holds `wbw_hd1k` and `wbw_hd1k_0` through `wbw_hd1k_5` — see
  the slice entry below, which supersedes the note this bullet first carried
  about slices 1-3 being undefinable.
- **`wbw_hd1k_1` through `wbw_hd1k_5` in `disks/diskdefs`**, and the measurement
  that shows why they used to look impossible. The arithmetic was never wrong:
  the directory at `1048576 + 8388608*n + 16384` is an ordinary CP/M directory
  for every `n` from 0 to 5, `hd1k_combo.img` is `1 MB + six 8 MB slices`
  exactly, and cutting each slice out with `dd` and listing it as a plain
  `wbw_hd1k` image gives 167/200/301/187/245/66 files and
  6144/6052/5112/5720/5592/7036 K free — used plus free is 8144K in all six.
  Reading the same six through the new definitions gives byte-identical files.

  What was wrong is the *build* of `cpmtools`, not the definitions. 2.23 cannot
  be configured without libdsk (`./configure` aborts with `No libdsk.h`), and
  `device_libdsk.c` flattens `(track, sector, offset)` into one linear sector
  number and hands it to `dsk_lread` with a geometry derived from `tracks`
  alone — 1024 tracks becomes 512×2×16 = 16384 sectors = 8 MB — so a linear
  sector at or past that is `DSK_ERR_BADPTR`, printed as `Bad parameter`.
  Measured, `tracks 1024`: offset `7340032` reads the directory, offset
  `8388608` does not. A `device_posix` build of the same 2.23 sources reads all
  six slices, which is what the counts above were taken with.

  Two consequences worth knowing. The ceiling is 8 MB **from the start of the
  image**, not from the offset, so `wbw_hd1k_0` cannot reach the last 1 MB of
  slice 0 on a libdsk build either — measured on a *copy*: writing a 5.5 MB
  file into slice 0 through `wbw_hd1k_0` fails with `can not write (null): Bad
  parameter` after 5242880 bytes and leaves a directory entry for a file that
  is not all there. Slice 0 holds 2000K today. And raising `tracks` to cover
  the offset is exactly the silently-wrong diskdef both disk scripts exist to
  warn about: `tracks 6144` with the slice-1 offset lists slice 1 correctly and
  then reports 47012K free on an 8 MB slice, so the next write would run off
  the end of the slice and into slice 2. The comment header in `disks/diskdefs`
  carries all of this; `todo.txt` carries what is left.
- **A `windows-latest` and a `macos-latest` job in `.github/workflows/test.yml`.**
  The core in `src/` is compiled in place by three downstream ports — `z80cpmw`
  with MSVC, `ioscpm` with Apple clang, `cpmdroid` with the NDK — and a change
  here that broke any of those compilers was found by that port's own CI, days
  later and in somebody else's tree.

  The `msvc` job compiles exactly what `z80cpmw.vcxproj` pulls out of this
  repository — `emu_init.cc`, `hbios_cpu.cc`, `hbios_dispatch.cc` — with the
  same `/W3 /std:c++17` and the same defines that project sets, against the
  pinned `cpmemu` clone for `qkz80`'s headers. Compile only: there is no MSVC
  build of the emulator to link and no test program that would run there. No
  `/WX`, deliberately — at `/W3` `cl` compiles the `qkz80` headers along with
  these files, and this job exists to answer "does the shared core still compile
  with `cl`", not to make a dependency's warnings red. (The six C4267 warnings
  `hbios_dispatch.cc` used to emit on its own now carry explicit casts; see the
  narrowing-warnings entry under **Changed**.)

  The `macos` job builds the CLI with Apple clang and runs the compiled test
  programs through the makefile's own targets. Not `make test`: the disk check
  compares bytes inside disk images and says nothing about the compiler, so
  running it twice would only add a second way to go red.

  Both jobs have since run. The first push, run `33011944119` on `5920681`, went
  red in the `macos` job at "Run the deterministic tests" — a real portability
  defect in `tests/cli_hostfile.cc`, which compared strings across a macOS
  symlink — and the fix is the commit after it. Run `33012239667` is green on
  all three jobs.
- **`make -C src qkz80-source`** — prints which of the four possible `qkz80`
  the build would use, without building anything.
- 31 more checks in `tests/hbios_hostname.cc` — 34 to 65 — covering `0xEA` and
  the `0xE1`/`0xE2` bound, and 22 more in `tests/cli_hostfile.cc` — 31 to 53 —
  covering the read name, the basename cap and the cap's two cut directions.

### Fixed

- **A `--boot` on the command line no longer rewrites the persisted boot
  target.** It was saved at exit like any other NVRAM state, so *any* automated
  run that booted the emulator — a test, a script, a CI job — silently replaced
  whatever the developer had configured with whatever that run happened to pass.
  It is an override for the run now. A setting the *guest* changed during the run
  is a different thing and is still saved, which is why the exit path compares
  rather than just testing a flag: `SYSCONF` leaves NVRAM holding something other
  than what `--boot` put there. A `boot` key in the config file is unchanged and
  still written through; that file is itself a persisted choice.
- **`HBF_HOST_OPEN_R` and `HBF_HOST_OPEN_W` refuse a path the guest never
  terminated** instead of using the first 256 bytes. Silently truncating turns a
  guest bug into an emulator action: on a read it opens some other file, and on a
  write it *creates* one, at a path nobody asked for and which
  `HBF_HOST_GETNAME` would then report as the destination. Nothing shipped can
  reach it — `R8` and `W8` both terminate inside 128 bytes — so this is for the
  guest program that is not one of those two. 255 characters and a terminator is
  the longest path that can be delivered.
- **`emu_host_path_basename()` caps its answer at 255 bytes, keeping the
  extension.** There was no cap, so a 5000-character last component became a
  5000-character suggested download name: refused by every filesystem in this
  family, and refused without saying why. What survives is the extension and the
  front of the stem, because `aaaa…aaa` with the `.txt` thrown away opens in
  nothing. A name that is nearly all suffix keeps its end instead, the same
  choice `HBF_HOST_GETNAME` makes, and a cut never lands inside a UTF-8
  sequence.

  The two cuts move in opposite directions, which the first version of this got
  wrong: a head-keeping cut backs *off* a UTF-8 continuation byte, and a
  tail-keeping one has to skip *forward* past it. Backing up on the tail adds a
  byte rather than removing one, so `x.` followed by 5000 e-acutes came back 256
  bytes long — one over the cap the same change documents in `emu_io.h`. It
  gives up a whole character now and returns 254. A guest command line need not
  be UTF-8 at all, so a tail of nothing but continuation bytes has no boundary
  to skip to; that one takes the last 255 bytes rather than running off the end
  and answering with the empty string. `tests/cli_hostfile.cc` covers both; the
  two cases already there each exercised one axis and neither exercised both.
- **`W8` tells a CP/M read error from end of file.** It treated every nonzero
  `F_READ` status as a clean EOF, which is exact under CP/M 2.2 — 1 is the only
  code it returns — and wrong under ZSDOS and CP/M 3, which report a read failure
  with codes of their own. A failed export looked like a complete one: the host
  file closed and `Done: <n> bytes` printed for a file that stops wherever the
  disk stopped answering. Silent truncation reported as success is the same
  defect the `1Ah` handling exists to undo.
- **`W8`'s and `R8`'s failed opens name the path under a label.** `W8` printed
  it bare, in the same shape as the `To host:` success line, which reads as a
  claim about a destination when nothing was created — and in a different case,
  because `W8` lowercases a name it builds from the FCB and does not touch a path
  the CCP shouted, so the same failure printed two different cases depending on
  which form of the command was used. Both now print `Error: Cannot ...` and then
  `  Asked for: <path>`, which is what it is. `R8`'s failed open printed no path
  at all and now prints the same two lines.
- **`src/makefile` says which `qkz80` it picked** when it falls through to
  `/usr/local`. That branch is reached by a machine with no `pkg-config` entry
  and no sibling `cpmemu`, it always succeeds, and what it links is a different
  Z80 core from the one this tree is developed against — silently, until
  something behaves oddly at run time. The sister path also resolves against
  this makefile rather than against the current directory now; the two agree for
  `make -C src` and for a root-level `make -f src/makefile`, so nothing has gone
  wrong yet, but it should not depend on where make was invoked from.
- **`disks/rebuild_disk_utils.sh` blames the permissions, not `cpmcp`.** `cpmrm`
  exits 0 on an image it cannot write, having removed nothing, so the next thing
  to speak was `cpmcp` with "file already exists" — a run that failed for a
  permission problem while pointing at the copy step and the diskdef. It checks
  the image is writable before touching it, and checks the directory rather than
  `cpmrm`'s exit code afterwards. Its "nothing was written - none of these is
  present" ending no longer fires when the images *were* present and every one of
  them failed; that was a second wrong diagnosis on top of the first.
- **The web page says when a disk it offered could not be fetched.** The failure
  was written into the loading overlay and then hidden two lines later by
  `hideLoading()`, and the emulator started anyway — a machine with no disk in
  it, stopping at `Boot [H=Help]`, which reads as a broken emulator rather than
  as a file that was not there. It is named in the terminal, where it stays, and
  in the status line. Starting is still right: the ROM boots to its own menu and
  a local image can be attached with the file picker.

  Read rather than run: neither `node` nor `emcc` is on the machine this was
  written on, so `tests/web_*.js` could not execute and the page was never
  loaded in a browser. The change is additive — a `diskFailures` array pushed
  from the two `catch` blocks already there, and a reporting block after the
  banner — so nothing that used to run is diverted, but *nobody has seen this
  render*. `todo.txt` says the same.

  **Not** fixed, and left in `todo.txt` because it is a policy question: nothing
  in `web/makefile` or in `release.yml`'s staging step copies a single `.img`,
  so a stock deb still 404s on both defaults.
- **The flaky console test was a race in the test, not the pty.** "The reserved
  key is consumed once, on a tty" in `tests/cli_console.cc` failed for the
  machine rather than for the code — 2 failures in 40 runs here, always with
  `timed out waiting for a byte that never arrived` and never with anything to
  show for it, and A/B'd against a reverted `emu_io_cli.cc` it was flaky both
  ways. It was diagnosed as the pty round trip in `run_on_pty()` and left alone.
  It is not the pty.

  `run_on_pty()` tells the parent it may type as soon as the child has raw
  mode on, and this case's first assertion — arming `^E` reports nothing
  pending yet — runs *after* that. When the parent's write wins,
  `emu_console_check_escape()` finds the `^E` already in the terminal, consumes
  it and answers true; the assertion fails, every later step reads one byte out
  of step, and the closing `expect('z')` blocks until the alarm. Reproduced
  deterministically by sleeping 50 ms in the child after the handshake: four of
  the case's five checks fail and the fifth times out, exactly the observed
  shape.

  Fixed by giving `run_on_pty()` a `before_input` callback that runs in the
  child *before* the parent is told to type, and moving that one assertion into
  it. The same 50 ms probe now passes; 150 runs clean, and 60 more under a
  six-way CPU load, against 2 failures in 40 before. Two smaller things came
  with it: the child sets `stdout` unbuffered, because the alarm kills it
  outright and every `PASS`/`FAIL` printed before a timeout used to die in the
  buffer — which is why this looked like a line-discipline problem for so long —
  and the comment in `run_on_pty()` now says which assertions belong in which
  half.
- **The installed page had no ROM either.** The page fetches its ROM by a bare
  relative name, `release.yml` staged `roms/` into a sibling directory, and the
  rendered page 404'd on `emu_avw.rom` — the only ROM its select offers.
  `roms/emu_avw.rom` is staged next to the page now. Measured the way the
  `vendor/` paths were: a copy of the staging layout served over http, with
  every `href` and `src` in the rendered page fetched. `romwbw.js`, the three
  `vendor/` files and `emu_avw.rom` answer 200; the seven `.img` names still
  answer 404, and which images a package should ship is still the policy
  question in `todo.txt`.
- **Aux/printer redirection survives the `sim>` escape.** `emu_io_cleanup()`
  fclosed the three files, and it is not only the exit path — `read_console_line()`
  calls cleanup and then init around every `sim>` prompt to get a cooked terminal
  back, and init reopens nothing. One press of the escape key would have ended
  `LST:`/`PUN:`/`RDR:` redirection for the rest of the run, silently, since every
  write there is best-effort. They close through `atexit` now. Nothing can reach
  this today — the whole family still has no caller, see `todo.txt` — which is
  exactly why it would have gone unnoticed on the day something did.
- **A built sibling `cpmemu` made every test binary die at start-up on macOS.**
  `src/makefile`'s sister-checkout fallback tested for
  `../../cpmemu/src/libqkz80.a` and then emitted `-L<that dir> -lqkz80`, and
  `-lqkz80` does not have to resolve to the file that was tested for: `make
  libs` in cpmemu leaves a `libqkz80.dylib` in the same directory and the
  linker prefers it. Measured on macOS 27 with both present — `otool -L` on
  `test_vda_keyboard` showed `/usr/local/lib/libqkz80.4.dylib`, which is the
  `install_name` an uninstalled dylib carries, and every test aborted with
  `dyld: Library not loaded` (exit 134) while `make -C src test` reported
  `TESTS FAILED` with every individual check printing `PASS`. `DYLD_LIBRARY_PATH`
  does not rescue it either: SIP strips `DYLD_*` when make spawns `/bin/sh`, so
  the binary run by hand works and the same binary run by `make` does not. That
  branch now names the archive, so the link uses the file the test found; anyone
  who wants the shared library still sets `QKZ80_LIBS`. CI is unaffected — it
  builds only `make libqkz80.a` and passes `QKZ80_LIBS` explicitly.
- **`roms/verify_romwbw_pin.sh` went red in any tree that followed the
  documented CI recipe.** Its two `find` calls pruned `.git` and `archive` and
  nothing else, and every job in `test.yml` and `release.yml` does
  `git clone https://github.com/avwohl/cpmemu.git` *in the workspace root* —
  which is the tree root the script is pointed at. `cpmemu/tests` holds
  seventeen hand-written Z80 `.bin` fixtures of a few hundred bytes each, so the
  script reported seventeen "too small to contain an HCB" mismatches against
  RomWBW v3.5.1 and exited 1 in a tree where nothing was wrong. Reproduced
  exactly that way here — a real `git clone` of cpmemu beside a copy of `roms/`,
  `FAIL: 17 mismatch(es)` — and clean afterwards.

  The prune now also covers a sibling checkout sitting in the tree root, and
  matches on two conditions rather than one. The path, not the name, because a
  name prune reaches any depth and `z80cpmw` keeps its own sources in a
  `z80cpmw/` subdirectory — which is exactly the tree the script is meant to
  read when it is pointed at that port. And the directory has to have a `.git`
  of its own, which is what makes it somebody else's tree rather than part of
  this one, so a tree root that happens to be *named* `cpmemu` is still checked
  in full.
  Both behaviours are measured: a cloned `cpmemu` beside `roms/` is skipped, a
  plain `z80cpmw/` directory holding a ROM is not.
- **`src/emu_io.h` lost its CRLF endings inside the `v1.36` tag.** `04ad2b6` —
  the commit that tag points at — edited that header through a text-mode round
  trip and rewrote all 438 of its lines LF. `emu_io.h` is a CRLF file, and so
  is `src/emu_init.h` beside it — the commit that fixed this says "like the
  rest of this repo's headers", which measured is too strong: two of the seven
  headers in `src/` are CRLF and the other five are LF. The point stands for
  this one, which was CRLF before `04ad2b6` and is CRLF again after it.
  `17cd380`, the first commit after the tag, restored them, and the counts are
  the whole proof:
  `git show <ref>:src/emu_io.h | awk '/\r$/{c++} END{print c+0, NR}'` gives
  `0 438` at `04ad2b6`, `445 445` at `17cd380`, and `478 478` at `HEAD` after
  `322ca8e` added to the file. The content diff against the pushed version is
  only the breadcrumb described under **Changed** below; every other file in
  that pushed range was audited and this was the only one affected, because the
  `.cc` files were edited in binary mode and kept their endings.

  This is not cosmetic here. Three ports compile `src/` in place, so `v1.36` as
  tagged hands them the one revision of this header where every line differs
  from the revision before it and from the revision after — a whole-file diff on
  their next sync, in the file that carries the build-contract comment they are
  being sent there to read. A second reason, beside the commits `main` is
  ahead by, not to publish a release from that tag.

### Changed

- **The three CDN tags carry subresource integrity**, with `crossorigin`. The
  concern is not that the CDN moves — `xterm@5.3.0` is an exact npm pin — but
  injection: a jsdelivr compromise or a TLS intercept putting arbitrary JS into a
  page that holds the user's disk images.

  Two of the three URLs had to change first, and the reason is worth keeping.
  They pointed at `lib/xterm.min.js` and `lib/xterm-addon-fit.min.js`, and
  **those files do not exist in the npm packages.** `xterm@5.3.0` ships
  `lib/xterm.js` already minified and no `.min.js` at all; jsdelivr synthesises
  the name and serves the real file with its own banner on top — a banner whose
  text is *"Do NOT use SRI with dynamically generated files"*. An integrity
  attribute over a CDN-generated file is a page that goes blank the day the
  generator changes. The tags now name `lib/xterm.js` and
  `lib/xterm-addon-fit.js`, which are byte-identical to the registry tarballs
  (checked against `registry.npmjs.org`, not only against the CDN), and the
  `.min.js` form was 266 bytes *larger* — the same bytes plus that banner — so
  nothing was given up. The offline half of that item is closed by the vendoring
  entry below, which removed these tags again; the paragraph is kept because the
  `.min.js` trap is still there for whoever updates `web/vendor/` next.
- **xterm is vendored into `web/vendor/`, and the page no longer talks to a
  CDN.** An installed deb had no terminal without internet: `release.yml` staged
  the page, the wasm and `roms/` and no xterm at all, so the emulator ran and
  nothing could be seen or typed. `web/vendor/` now holds `xterm.css`,
  `xterm.js`, `xterm-addon-fit.js` and both packages' `LICENSE`, taken from the
  npm registry tarballs rather than from a CDN, and the six tags across
  `romwbw.html-template` and `romwbw-debug.html` point at them. `release.yml`'s
  staging step and both `web/makefile` deploy targets copy them.

  The bytes are provably the ones that used to be fetched: the sha384 of each
  vendored file equals the `integrity` value its tag carried. Those three
  hashes, the tarball URLs and the `.min.js` trap are recorded in
  `web/vendor/README.md`.

  There is no `integrity` attribute on the new tags and that is not a step
  backwards — SRI is a check on a file fetched from a host you do not control,
  and a same-origin file shipped inside the package *is* what the hash would
  have been taken over. The page and `web/vendor/README.md` both say so, so the
  next reader does not "restore" them.

  What could not be checked here: `emcc` is not on this machine, so the wasm
  could not be rebuilt and no browser has loaded the page since the change. The
  relative paths *were* checked without one — a copy of `release.yml`'s staging
  layout served over http, every `href` and `src` in the rendered page fetched,
  and `vendor/xterm.css`, `vendor/xterm.js` and `vendor/xterm-addon-fit.js`
  answer 200 with the right sizes. That was the stated reason the vendoring had
  not been done; it is now measured rather than assumed, but a look at the page
  is still on the browser list in `todo.txt`.
- **`release.yml` pins emscripten** (`EMSDK_VERSION: 6.0.8`) instead of
  installing `latest`, for the reason `CPMEMU_REF` beside it is pinned and
  `test.yml` pins `UM80_VERSION`. It has already broken one release: `13d1c23`,
  where `emcc` stopped implying the C++ driver at link time and the web build
  failed on a commit that had not touched it. Nothing in this repository builds
  the wasm — `emcc` is not a build dependency here — so CI is the only place that
  failure can appear, which makes the pin worth more here than anywhere else.
  6.0.8 is what `latest` resolves to as this is written, so the pin changes
  nothing about what the next release builds with; it only stops the answer
  moving on its own.
- **The build turns on `-Wimplicit-int-conversion` and `-Wshorten-64-to-32`,**
  and the 89 diagnostics they raised in this tree carry an explicit cast
  instead. Both flags are Clang-only and both are probed with `$(shell ...)`
  before being added, the way `cpmemu/src/makefile` guards its own: GCC does not
  ignore an unknown `-W` flag, it fails the compile, and a hardcoded one has
  stopped every GCC build in this family before.

  `-Wimplicit-int-conversion` is Clang's name for what MSVC reports as C4267,
  and the six warnings `z80cpmw` suppresses on its side turned out to be six of
  89, on 86 lines across six files. Every one is a deliberate narrowing — a
  `size_t` or `int` index truncated into a 16-bit guest address, which is the
  Z80 64K wrap HBIOS wants, or an `int` expression stored into an 8-bit
  register — so each site got a cast that says so, in the file's existing
  `(uint16_t)(...)` idiom, and no behaviour changed. `-Wconversion` is still
  deliberately out: it adds the core's intended mod-256 wraps on top, which is
  the call cpmemu made too.

  Two warnings per object file remain and they are not this repository's:
  `qkz80_MK_INT16` in `cpmemu/src/qkz80_types.h` builds an `int` and hands it to
  a `qkz80_uint16` parameter, so `qkz80_reg_pair.h:32` and `:35` are reported by
  every file that calls `set_low()`/`set_high()`. `src/makefile`'s comment says
  where the fix is; `todo.txt` carries it as an open item. `z80cpmw`'s
  `/wd4267` on `hbios_dispatch.cc` is now removable.
- **The macos CI job runs `tests/cli_console.cc` too.** It was left out with a
  justification that was already stale in the commit that wrote it: the job said
  the failure was "the pty round trip in `run_on_pty`", and that same tree's
  `cli_console.cc` said in as many words that the pty round trip was never the
  problem. The race was an assertion about an *empty* terminal running after the
  handshake that lets the parent type, and the `before_input` hook fixed it —
  see the Fixed entry above. Re-measured before adding it: 400 consecutive runs
  on macOS 27 arm64 with Apple clang 21, 0 failures. The job's comment now says
  that instead. The disk check stays out of the macos job on purpose: it
  compares bytes inside images and says nothing about the compiler.
- **`disks/verify_disk_utils.sh` asserts the `HBF_HOST_CAPS` interlock**, which
  `docs/RELEASE_ORDER_2026-08-25.md` asked for and nothing implemented. Matching
  the source was not enough on its own: an image is edited in place, so a
  rebuild that restores an older `w8.com`, or an image recovered from before
  v1.36, is a `W8` that hands a guest-supplied host path to a front end that has
  promised nothing. The check searches the copy *inside each image* for the
  three bytes `06 e9 cf` — `ld b,H_CAPS` then `rst 8`, the whole of
  `check_host_path_safe` — and asks before the staleness comparison rather than
  after, because the layout and byte checks both bail on a mismatch and the two
  questions are different. Nothing in the file's text discriminates: the armed
  1408-byte `w8.com` still in this repository's history prints the same
  `Usage: W8 <cpmname> [hostpath]` as the 1792-byte interlocked one. Verified
  both ways — the two tracked images pass, and the `w8.com` recovered from
  `de85946` fails.

  Done with `od` rather than `xxd -p | grep`: `xxd` is not on a stock Ubuntu
  runner, and a flat hex string also matches across a byte boundary
  (`b0 6e 9c f3` reads as `b06e9cf3`).
- **`README.md` warns beside the disk-image table** that these images hand a
  host path to whichever emulator opens them, and that an `ioscpm` build before
  52 must not be given one. `docs/RELEASE_ORDER_2026-08-25.md` step 0 has asked
  for this since it was written: it is the only measure that reaches someone who
  already has the file, because they read the repo rather than re-clone.
- **The deliberate link error names the notice to read.**
  `emu_host_path_caps()` being undefined in the core is the only *push* this
  repository has — the other two channels, `DOWNSTREAM.md` and the verify
  scripts, both wait to be pulled — and it was a dead end. The comment a
  maintainer lands on when they go to define the symbol named no notice, and
  `DOWNSTREAM.md`'s index entry mentioned neither the link error nor the steps
  no compiler enforces, so the error could be silenced with a one-line stub by
  someone who then missed the disk-image refresh, the `R8` destructive-delete
  fix and the release ordering entirely. `src/emu_io.h` now says it outright at
  the point of arrival — "If you reached this comment from a linker error
  ('undefined symbol emu_host_path_caps'), that error is the intended signal
  that your port has a core sync to absorb. Do NOT just add a stub to make it
  compile: read docs/DOWNSTREAM_2026-08-25.md first" — and `DOWNSTREAM.md`'s
  index was rewritten to lead with the build-contract change and to list the
  non-code steps beside it (`17cd380`). The index leads with
  `docs/DOWNSTREAM_2026-08-26.md` now, the read-name sync, because `322ca8e`
  added a second contract change behind the first.
- **`docs/RELEASE_ORDER_2026-08-25.md` is corrected against the four working
  trees**, since three other repositories point at it and were being told the
  old state. Step 0 said "committed locally, NOT pushed" with `origin/main` at
  `fc68ca0`; `main` and `v1.36` are both on `origin` (`refs/tags/v1.36` peels to
  `04ad2b6`, and `main` is **six** commits past it — `git rev-list --count
  v1.36..HEAD` = 6; this entry said five, which was the count before the commit
  that wrote this entry landed) and `HEAD` no longer serves an armed `W8`.
  Step 1 said `ioscpm`'s fix was uncommitted; it is `bb5543f`. Step
  2 said `cpmdroid` was not checked out; it is, at `c6756af`, with its code half
  done. Step 3's named change landed in `z80cpmw` `2f10d4c`, but that port does
  not define `emu_host_file_get_read_name()`, which the post-v1.36 core calls
  unconditionally — so it will not link, and the doc now records that. And a
  section was added for a second data-loss path the document never mentioned:
  `ioscpm`'s `checkCatalogVersionAndInvalidate()` deletes every `.img` in
  `Documents/Disks` on any change to `disks.xml`'s `version` attribute — user
  imports and app-created disks included, which the catalog cannot restore —
  and step 4 is what tells you to bump that attribute.
- **`todo.txt` went from 269 lines to 127**, and holds only work somebody could
  start. Closing an item had been producing a paragraph explaining that it
  closed, longer than the item was, so roughly two lines in three were not open
  work: post-mortems of fixed bugs, re-readings of parity tables, and
  "the cleanup half is fixed" sentences that are already in the commit that
  fixed them. Those are deleted rather than summarised — that summary is the
  disease.

  Three claims in it were false at `HEAD` and went with them: that `node` is not
  on `PATH` and `make -C src test` therefore prints `SKIP web JS tests` (it is,
  and it does not — both JS suites run and pass), that the `macos` and `msvc`
  jobs "have never been executed" (both ran the day the sentence was written;
  see the CI entry above), and a fourteen-line restatement of the `cpmtools`
  libdsk 8 MB ceiling, which is recorded in more detail in `disks/diskdefs`'
  own header and again in `.claude/CLAUDE.md`.

  Every surviving item now carries a tag — `[BROWSER]`, `[EMSCRIPTEN]`,
  `[DECISION]`, `[RELEASE]`, `[MAC/LINUX]`, `[WINDOWS]` — saying what a machine
  has to be able to do to take it, and cites a function or a greppable symbol
  rather than a `file:line` that rots within hours.
- **`MANUAL_CHECKS.md`** — the checks that need a person at a keyboard, which is
  all of the web keyboard pass, the vendored xterm coming up at all, and the two
  `W8` behaviours only a browser shows. `todo.txt` keeps one line pointing here.
  Items are deleted once someone runs them; the file is not a log of checks that
  were done. Same convention as `cpmemu` and `z80cpmw`, which already had one.

## [1.36] - 2026-08-25

`VERSION` was bumped in `48f8c19` and tagged here. It follows `v1.35`.

Downstream ports do not need this tag — they build these sources out of a
sibling working tree, so they had the core changes at the commit. What the tag
is *for* is this repository's own users: the deb/rpm CLI binary, and the web
build, whose wasm nothing in this tree can produce (emcc is not a build
dependency here). The browser half of this release — the shared basename
reduction, the zero-byte download, a refused disk reported as refused, and
`W8`'s whole `To host:` change — reaches a web user only when CI builds it.

**Correction, 2026-09-01: none of that was ever built.** No release workflow ran
for this tag — the newest run of "Build and Release Packages" is `31186495366`
on 2026-08-07, and `gh release list` shows `v1.35` as `Latest` — so the deb, the
rpm and the wasm this section promises "this repository's own users" do not
exist and the browser half reached nobody. The tag stands; the packages ship for
the first time in `[1.37]` above.

The disk images are a separate channel again and are **not** in this release:
`release.yml` stages the binary, the web page and `roms/` only. The refreshed
`r8.com`/`w8.com` reach the mobile and Windows ports through `ioscpm`'s release
assets, which all three catalogs point at — and the ordering constraint in
[docs/DOWNSTREAM_2026-08-25.md](docs/DOWNSTREAM_2026-08-25.md) applies to that
refresh: the sanitiser fix has to land in `ioscpm` before, or with, the new
images.

### Added

- **`W8` says where the file actually went** — new HBIOS extension function
  `HBF_HOST_GETNAME` (0xE8): `C` = buffer size at `DE`, `A` = 0 and the buffer
  holds the effective destination. It used to echo the path the user typed,
  which is never the path that gets written. The CCP uppercases the whole
  command line, so the emulator has to resolve the directory case-insensitively
  and lowercase the file name; and on the browser and the sandboxed mobile
  ports there is no outer-OS path to honour at all, so the file lands in a
  download folder or the app's own Exports area under a name the guest never
  sees. `To host: /HOME/ME/OUT.TXT` named nothing on three of the five front
  ends. On the CLI the answer is now absolute and `realpath`-canonical, so it
  names a place rather than a name. A failure is not an error: an emulator
  built before 0xE8 existed answers "no such function" and `W8` falls back to
  printing what was asked for, so a current `w8.com` still runs on an
  already-released front end. `getTrapTypeFromFunc()` widened its extension
  range from 0xE0-0xE7 to 0xE0-0xEF to route it.
- **`W8` refuses to send a host path to an emulator that cannot promise it is
  safe**, via a new `HBF_HOST_CAPS` (0xE9): no inputs, no state, so a guest can
  ask it *before* opening anything. An emulator predating it answers `0xFF` and
  `W8` stops without opening, creating or truncating a thing.

  The bit is supplied by a backend function `emu_host_path_caps()` that
  `emu_io.h` declares but the core does **not** define, so a port that syncs
  this core without confirming its own path safety fails to *link* rather than
  assert a guarantee it has not made. It means "a guest path is never used
  destructively", not "confined to one directory", so the CLI and browser
  backends both set it honestly (the CLI writes exactly the named file); a
  backend that cannot yet promise even that returns 0 and W8 withholds the path
  form.

  This is the part of the fix that does not depend on anyone's release order.
  Disk images and the emulator that runs them travel separately — the front
  ends fetch images from a pinned release tag, but nothing stops an image being
  copied in by hand — so a `W8` able to send a path can meet a front end that
  mishandles one. On iOS before its build 52 that was fatal. The interlock makes
  that combination refuse instead of relying on it never happening.

  `HBF_HOST_GETNAME` could not serve as the probe: "no such function" and "no
  write file open" are both `0xFF`, so it can only be asked once a file is
  already open.

  Only the path form is withheld. `W8 FOO.TXT` with no path still works on any
  emulator, because the name then comes from the FCB and the CCP cannot put a
  `.` in an FCB name field — so it can never be `..`. **A refreshed disk image
  therefore still does ordinary exports on an old front end;** only the
  dangerous form is refused. It fails closed: the probe cannot tell a safe old
  emulator (the CLI, never vulnerable) from a dangerous one, and the cost of
  guessing wrong one way is a message, the other way is the user's disk library.
- **`emu_host_path_basename()`** in `emu_io_common.cc`, for the front ends with
  no filesystem to honour a directory with. Takes **both** separators, because
  the string comes off a guest command line that may have been typed on any
  host, and never returns `""`, `"."`, `".."` or a bare drive letter. There
  were three different answers before: the browser split on `/` only, and the
  iOS backend did not split at all.
- **`R8` and `W8` take the whole rest of the command line as the path.** Both
  stopped at the first space, and `/Users/me/My Documents` and
  `C:\Program Files` are ordinary paths on two of the five hosts. Trailing
  spaces are trimmed.
- **`emu_rename()`** in `emu_io.h`, beside the `emu_fseek`/`emu_ftell` pair.
  ISO C leaves `rename()` undefined when the target exists and both the MSVC
  CRT and mingw's msvcrt refuse it outright, so `emu_file_save()` — which
  renames a temp file over the image precisely so a failed write cannot destroy
  the previous one — had the safe path be the broken one on Windows. Asked for
  by `z80cpmw`, which had already worked around it locally.
- **CI runs the tests.** `.github/workflows/test.yml`: build, `make -C src
  test`, and then an assertion that the disk check did not silently skip. There
  was no test step anywhere before; `release.yml` builds and packages only.
- `tests/hbios_hostname.cc` (29 checks: the `HBF_HOST_GETNAME` buffer bound,
  the not-writing and empty-name answers, and both ends of the widened function
  range) and 20 more checks in `tests/cli_hostfile.cc`.
- `disks/rebuild_disk_utils.sh` — assembles both utilities and installs them
  into every tracked image. The recipe existed only as prose, in three places
  that had already drifted from each other, and it did not work as written:
  `cpmcp` will not overwrite, so the old copy has to be removed first.

- **`W8` takes a host path, the way `R8` already does** — `W8 <cpmname>
  [hostpath]`. It used to read only the default FCB, so it could write exactly
  one place: a lowercased 8.3 name in the emulator's working directory. With no
  path it still does exactly that. The case handling is the interesting part:
  the CCP uppercases the whole command line, and `R8` recovers by retrying the
  path case-insensitively, which works because the file it wants exists. A write
  cannot — the last component is the file being created and will never match —
  so `resolve_write_path()` resolves the parent, which does have to exist, and
  lowercases the basename. On the web build a path cannot mean anything, so the
  basename becomes the suggested download name.
- **`--escape=none` really reserves nothing** (also `off`, `^@`), matching
  `z80cpmw`'s `keyboard.ctrlRToCpm`. It was previously a trap rather than a
  no-op: the literal branch took the first byte, so it quietly reserved the
  letter `n`. Both parse branches insist on an exact length now, which also
  rejects `--escape=non` and a multi-byte literal that used to truncate to its
  UTF-8 lead byte.
- **Console tests on a real pipe and a real pty.** `tests/cli_console.cc` —
  44 checks over 12 cases, each in a forked child so the statics in
  `emu_io_cli.cc` start clean. The pair that matters: a piped `"a\nb\n"`
  delivers two CRs, because a script's lines end with LF and `c_iflag` never
  applies to a pipe; on a tty Enter arrives as `0x0D` and Ctrl+J as `0x0A`,
  because `emu_io_init()` clears ICRNL and the rewrite has to be *absent* there
  or the two keys collapse into one. Either half alone still looks right in
  ordinary use, which is what makes it regress quietly, so both are checked and
  the tty pair is checked twice — once through the blocking read, once through
  the peek path.
- **A check that the disk-resident `R8`/`W8` match their source.**
  `disks/verify_disk_utils.sh` assembles each utility, extracts the copy each
  tracked image holds and compares, wired into `make -C src test` and skipping
  when `um80`, `ul80` or cpmtools are absent. Nothing builds these `.COM` files
  as part of any target — they were assembled once and copied into the images —
  which is exactly how `w8.com` in `hd1k_infocom.img` came to be stale.
- `tests/web_console_output.js` (12 checks, including a sweep of all 256 byte
  values) and `tests/vda_keyboard.cc` (15 checks).
- `HBDisk::total_sectors()`, named and placed to match
  `MemDiskState::total_sectors()` so the two kinds of disk read alike at the
  call sites.

### Fixed

- **`W8` truncated every binary export, silently.** It stopped at the first
  `1Ah` byte. `1Ah` is `LD A,(DE)` and appears in almost any `.COM` file:
  exporting `W8.COM` itself produced **368 bytes of 1408**, reported as
  `Done: 368 bytes`. It now copies the file whole and drops only the run of
  `1Ah` at the very end — the padding CP/M writes into a file's last record,
  and exactly what `R8` puts there on import. So a text file imported with `R8`
  still comes back byte for byte, and a binary containing `1Ah` no longer
  loses everything after the first one. A file whose real content ends in `1Ah`
  loses that tail; CP/M stores no length, only whole 128-byte records, so
  nothing can tell the two apart. That is the documented boundary.
- **`R8` erased unrelated CP/M files.** It copied the host basename into the
  FCB unfiltered, and `?` and `*` make an FCB *ambiguous* — then handed that
  FCB to `F_DELETE` before `F_MAKE`. Importing a host file called `a?b.txt` did
  not create one CP/M file, it deleted every file matching `A?B.TXT` first, and
  said nothing. Verified live: two unrelated files went with it. Illegal
  characters now become `-` and `R8` says when it substituted.
- **A host name containing `_` imported as a file nothing could reach.**
  Underscore is in the CP/M 2.2 CCP's delimiter set, which is not obvious —
  `my_file.txt` imported as `MY_FILE.TXT`, and then `DIR MY_FILE.TXT` said
  `NO FILE` while `DIR MY?FILE.TXT` listed it, `ERA` could not remove it,
  `TYPE` printed `MY_FILE.TXT?`, and `W8` could not export it because the CCP
  parsed the argument as `MY`. The data was on the disk and unreachable, and a
  directory slot was consumed permanently. `_` is rejected along with the rest
  of the delimiter set now, and the substitute is `-`, which is not one — the
  first version of this fix substituted `_` and made the problem worse.
- **`R8` destroyed a CP/M file when handed a host directory.** `fopen` on a
  directory succeeds on Linux and macOS — only the first read fails — so the
  open reported success, `F_DELETE` and `F_MAKE` ran, and the guest was left
  with an empty file and `Done: 0 bytes`, which is what a legitimately empty
  import also prints. The CLI backend refuses a directory now, before the open
  can report success.
- **`R8` announced a CP/M file it had not created**, printing `Creating: NAME`
  before opening the host file.
- **`R8` made CP/M files nothing could address.** Two ways. A path ending in a
  separator reached `F_MAKE` with the FCB's eleven blanks intact and created a
  nameless directory entry; and the extension copy stopped only at the NUL, so
  `a.b.c` produced a file named `A` with type `B.C`, which the CCP's own parser
  cannot name because it reads the first dot as the delimiter. The type now
  comes from the last dot and the name stops at the first, so
  `archive.tar.gz` is `ARCHIVE.GZ` and `notes.2024.txt` is `NOTES.TXT`; a
  leading dot is part of the name, so `.profile` is `PROFILE`; and a path that
  names no file is refused before anything is opened or deleted.
- **`R8` reported success for a short import.** Every full record write was
  checked and the final partial one was not, so a CP/M disk that filled on the
  last record printed `Done`. `F_CLOSE`'s status was discarded too, and
  `F_CLOSE` is where CP/M writes the directory entry back.
- **The byte count `R8` and `W8` print was the low 16 bits**, so a
  100000-byte transfer reported `34464`.
- **The `ORG` came out of both utilities.** `org 0100h` was wrong for this
  toolchain: M80 assembles each as one relocatable code segment and L80 bases a
  `.COM` at 0100h *by itself*, so the ORG was applied on top of that base and
  pushed the code to 0200h behind 256 zero bytes. It ran only because CP/M
  loads the whole file at 0100h and the Z80 slides through the NOPs. `w8.com`
  was built that way and `r8.com` was not, which is the only reason
  `verify_disk_utils.sh` could not check both.
- **`verify_disk_utils.sh` exited 0 after verifying nothing**, and `make test`
  called that a pass. A wrong diskdef, a renamed image or a deleted source all
  land there — and a wrong diskdef is not loud, cpmtools prints a garbage
  directory rather than failing. An image *missing* a utility was an info line
  and a pass too, which is precisely the shape the stale `w8.com` incident
  took; it is a failure now, and CI asserts the count rather than the verdict.
- **`HBF_HOST_GETNAME` could report a path that names something else.** `C` is
  one byte, so a destination longer than 254 characters did not fit — and a
  bare name gets the whole working directory prepended, so a deep checkout
  reaches that. Clamping handed back a path chopped mid-component that `W8`
  then printed as fact, which is the failure the call exists to remove. It
  keeps the *end* of the path now, where the file name is, behind a leading
  `...` that makes the answer read as a fragment.
- **`HBF_HOST_GETARG` wrote its terminator outside the buffer** for an argument
  longer than 255 characters: the copy was clamped and the NUL was not, so the
  guest got an unterminated buffer and a zero byte dropped past the end of it.
  Unreachable here (nothing calls `setHostCmdLine`), reachable from any port
  that wires it up.
- **The browser dropped a zero-byte export** and kept the CCP's uppercase in
  the download name while the CLI lowercased it. An empty CP/M file is a real
  file, and the same `W8` command should not produce differently-named files on
  different front ends.
- **The web page reported "Disk N loaded" for a disk the core refused.**
  `loadDiskData()` discarded `_romwbw_load_disk`'s return value, so a stray
  small file dropped into the slot looked like a successful load with the only
  trace in the JS console — and `reloadDisks()` would push it back in on the
  next ROM change.
- **The shared core accepted a 0-byte disk image** and mounted it as an empty
  disk. `z80cpmw` reported the same hole on its side and asked for it to be
  closed in both.
- **`emu_file_load_to_mem()` still used the bare 32-bit `fseek`/`ftell` pair**
  the rest of that file was hardened away from in v1.35 — the last site.
- **`^E` was stolen twice over.** It is the `sim>` escape, and the blocking read
  both latched it *and* returned it to the guest, so one keypress moved the
  WordStar cursor (CP/M 2.2 reads `^E` as physical end of line) *and* froze the
  emulator. `emu_console_read_char()` returns the new `EMU_CONSOLE_RETRY` and
  `CIOIN` rewinds over the two-byte `OUT (0xEF),A` the way its non-blocking
  branch already did, so the guest gets no byte at all.
- **POSIX raw mode clears IXON**, so `^S` and `^Q` reach the guest instead of
  being XON/XOFF flow control — `^S` is WordStar cursor-left and `^Q` prefixes
  the whole second half of its command set.
- **`VDAKST` said "no key" however much was queued.** It set the pending count
  in `E` but left the status byte in `A` at zero, and `A` is what a caller
  tests, so a guest polling the video keyboard could never get past the status
  call. Its `CIO` twin, `CIOIST`, has always set both.
- **`VDAKRD` handed the guest a stale byte.** With no key pending it flagged the
  wait and returned *without rewinding PC*, so the Z80 proxy's `RET` fired
  immediately with `E` holding whatever the previous call left there: the guest
  took a stale byte for a keystroke and never came back for the real one.
  `VDAKRD` is now the same code as `CIOIN`, which has rewound since the
  non-blocking path was added. Both bugs are reachable from every port —
  `SYSGET_VDACNT` reports one VDA whatever the front end.
- **The MSVC build was broken by `v1.35`.** That release started measuring files
  in 64 bits and reached for `fseeko`/`ftello`; Visual C++ provides neither, so
  the Windows port stopped compiling the moment it took the release, while
  `docs/DOWNSTREAM_2026-08-07.md` told that port it "builds unchanged". The
  seek/tell pair joins `emu_sleep_ms` and `emu_strcasecmp` in `emu_io.h`, with
  `_fseeki64`/`_ftelli64` on MSVC. The offset type comes from the same place:
  MSVC's `off_t` is a 32-bit `long`, which would truncate past 2 GB — inside the
  range a combo disk image reaches.
- **A disk size was truncated silently.** Two sites computed `disk.size / 512`
  into a `uint32_t` from a `size_t`. A truncating conversion keeps the
  remainder, so an image just past 2 TiB would report as nearly *empty* rather
  than as huge, and `DIOCAP` would hand the guest a capacity with no relation to
  the disk. It clamps at `UINT32_MAX` now, which is the honest answer given
  HBIOS deals in 32-bit sector counts.
- **The web frontend starved xterm.js.** `Module.onConsoleOutput` passed CR, LF,
  BS, ESC and `0x20–0x7E` and dropped everything else — 157 of the 256 byte
  values, with BS altered rather than dropped. TAB never aligned a column, BEL never rang, FF never cleared, no byte
  with the high bit set arrived, and BS was rewritten `\b \b`, a *destructive*
  backspace, so a guest moving the cursor left erased the character it moved
  over. xterm.js is a more complete VT than any native front end in this family;
  this filter was what made it look otherwise. It forwards every byte now,
  keeping only the LF → CR LF rewrite.

  Corrected since: that is true of `Module.onConsoleOutput` and false of the
  path as a whole. `emu_console_write_char()` in `emu_io_wasm.cc` masks to
  `0x7F` before the byte ever reaches JavaScript, so no high-bit byte can reach
  xterm today whatever the page does. The mask is an open question in
  `todo.txt`, not a regression from this change.
- **`Module.onError` is implemented.** `emu_io_wasm.cc` has always called it and
  nothing ever defined it, so every error the core reported went nowhere — a
  large part of why the dead VDA/sound wiring survived unnoticed, since the one
  channel that would have complained was itself unplugged. That wiring is
  deliberately still not fixed; see `todo.txt`.
- **The web frontend forgot "Don't warn" across a ROM change.** `reloadDisks()`
  restored `is_manifest` but not `warning_suppressed`, and the ROM select's
  change handler never reaches the start path, which was the only other place
  the checkboxes were pushed into the core — so a user who ticked "Don't warn"
  and then picked a different ROM got the overwrite modal back with the checkbox
  still ticked in front of them.
- **`hd1k_infocom.img` carried a stale `w8.com`.** Its copy was byte-identical
  to combo's pre-fix one. Verified by running it, not by writing the file:
  booting combo with infocom as disk1 puts it on `G:`, and `W8 ZORK1.COM
  EXPDIR/ZORK.BIN` wrote `ExpDir/zork.bin`, 587 bytes. Mind the diskdef —
  `hd1k_combo.img` is `wbw_hd1k_0` because of its 1 MB MBR prefix, the plain
  8 MB `hd1k_infocom.img` is `wbw_hd1k`, and cpmtools with the wrong one prints
  a garbage directory rather than failing, which is how infocom was briefly and
  wrongly recorded as having no `w8.com` at all.
- The dead slice selects are removed from the web page.

### Changed

- `docs/DOWNSTREAM_2026-08-07.md` — the `v1.35` notice itself — carries a dated
  correction to its claim that `z80cpmw` "builds unchanged", when that port did
  not build at all. The port's sync commit was then made on a machine with no
  Windows toolchain, and that note is what made it look safe. The correction is
  in the notice rather than in `DOWNSTREAM.md`, because the notice is what a
  port re-reads when it takes the release.
- `verify_disk_utils.sh` reported `r8.com` as **not comparable** rather than as
  stale, because the two utilities were not built the same way. That is settled
  now — see "The `ORG` came out of both utilities" below — and the question it
  left open is answered: `r8.asm` **does** still correspond to the shipped
  `r8.com`, byte for byte in both images. The check covers all four copies.

## [1.35] and earlier

Not written up here. See `git log` — the commit messages are the record.
