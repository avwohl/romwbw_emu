# Release order for the W8 host-path work

**The constraint this whole document exists for:** refreshing the published
disk images is what arms a data-destruction bug in `ioscpm`. The images and the
port fixes cannot go out in either order — the fixes go first.

This is the coordination plan across four repositories. The technical detail is
in [DOWNSTREAM_2026-08-25.md](DOWNSTREAM_2026-08-25.md); this file is only the
ordering, and why it is that order.

> ### Update: the interlock helps, but it is not a security boundary — the order still matters
>
> An earlier revision of this block claimed the `W8` capability interlock
> (`HBF_HOST_CAPS`, 0xE9) made the dangerous combination fail safe "whatever
> route the image took". That was too strong, and an adversarial re-check
> knocked it down. Two facts, both reproduced:
>
> - **The interlock is advisory, not enforcing.** It lives inside `W8.COM`, but
>   the trust boundary is HBIOS function `0xE2`, and the core does not sanitise
>   there. A 384-byte CP/M program that calls `0xE2` directly skips the probe
>   entirely — verified: such a program, on the newest fully-interlocked
>   emulator and image, wrote a file *above* its working directory. Disk images
>   are downloaded content, so this is the guest defending against the guest.
>   The only layer that can actually contain a host path is the **front end's
>   backend** (`emu_host_file_open_write`) — which is exactly steps 1, 2 and 3.
>   The interlock protects an *honest* `W8` user from a footgun; it protects
>   nothing from a crafted `.COM`.
> - **The armed `W8` is already public and the interlock cannot reach it.** The
>   protection ships in the `.COM`, so it only helps images built *after* the
>   interlock. The images at commits `98eb6a1`..`fc68ca0` carry a
>   host-path-capable `W8` with no probe, public since 2026-08-24. Upgrading
>   the emulator does nothing for a user who already has one of those images.
>   *(2026-08-27: those
>   commits are still ancestors of `main`, so the blobs remain published — but
>   `HEAD` no longer serves one. See step 0.)*
>
> So the interlock narrows one cell of the matrix below — an honest `W8 <f>
> <path>` on a *refreshed* image meeting an old front end — and leaves the rest.
> What it does **not** change:
>
> - **Step 1 is not merely first, it is the only retroactive remedy.** The
>   backend sanitiser inspects whatever string arrives, so it defangs every
>   `W8` ever built and every crafted `.COM`. It is the only fix that reaches
>   the already-public images. Ship `ioscpm` build 52.
> - **The core sync itself is now a gate for `cpmdroid` and `z80cpmw`.**
>   `HOST_CAP_SAFE_PATHS` is asserted by shared core code unconditionally, with
>   no backend hook, so a port that compiles the v1.36 core for any reason
>   starts *claiming* safety whether or not it sanitises — and `W8` believes it.
>   Before the interlock, the core version had no bearing on path safety at all.
>   As of the `emu_host_path_caps()` refactor this is now enforced by the
>   linker, not by prose: `HBF_HOST_CAPS` calls a backend function the core does
>   not define, so **neither port can build against the v1.36 core until it adds
>   `emu_host_path_caps()`** — and adding it is the moment to confirm the backend
>   is not destructive. See the design note at the end of this file.
> - **`R8` gained no interlock.** Its host path is unprobed, and its own
>   destructive bug (an ambiguous FCB handed to `F_DELETE`) is live on every
>   port today and fixed only by the step-4 image refresh. So step 4 still may
>   not precede step 1's `R8` fix reaching users.
> - **The `v1.4.5` rule is unchanged**, though its worst case for the `W8` half
>   softened from data-loss to a broken feature. See that section.

> ### Update 2026-08-27: four of the six steps have moved, and step 5 has a second data-loss path
>
> Everything below was re-checked against the working trees on this machine and
> against `origin`. The steps themselves are corrected in place; this block is
> the summary of what changed, because three other repositories point at this
> file and were being told the old state.
>
> - **Step 0 is done.** `main` and the `v1.36` tag are both on `origin` —
>   `git ls-remote` gives `refs/heads/main` = `a95db9f` and `refs/tags/v1.36^{}`
>   = `04ad2b6`, with `main` five commits past the tag. The old step 0's central
>   claim — that the public repository serves the armed host-path `W8` with none
>   of the protection — is no longer true of `HEAD`. Both tracked images carry
>   the interlocked `W8`, and `disks/verify_disk_utils.sh` now asserts the
>   `06 e9 cf` probe on each rather than leaving it to whoever remembers.
> - **Steps 1 and 2 are done in code.** `ioscpm` committed the build-52 fix as
>   `bb5543f` and has moved on to `15f48e9`. `cpmdroid` *is* checked out here,
>   at `c6756af`, and its code half is complete.
> - **Step 3 is half done and will not link.** The `z80cpmw` change this file
>   names landed in `2f10d4c`, but that port does not define
>   `emu_host_file_get_read_name()`, which the post-`v1.36` core calls
>   unconditionally. *(2026-09-01: it does now - `713bfce`, the same day this
>   block was written. See step 3.)*
> - **Step 5 has a second data-loss path this document never mentioned**, and it
>   is fired by step 4's own instruction to bump the catalog `version`. It has
>   its own section below.
> - **What has not changed:** the armed `W8` blobs are still in git history —
>   and now *published*, because pushing `main` published them — and the usage
>   string still does not tell an armed `W8` from an interlocked one. There is
>   also still no `v1.36` GitHub release: `gh release list` shows `v1.35` as
>   Latest, published 2026-08-07. *(2026-09-01: still true, and now settled -
>   there will never be one, and `v1.37` is cut from `main` instead. See
>   step 0.)*

---

## The hazard in one paragraph

`W8 <cpmfile> [hostpath]` sends the host path to the emulator verbatim.
`ioscpm` stores it unsanitised as the export *filename*
(`emu_io_ios.mm:404-410`) and then builds a destination with
`exportsDir.appendingPathComponent(name)` followed by `try? fm.removeItem(at:)`
(`EmulatorViewModel.swift:1906-1908`). `appendingPathComponent` does not escape
`..`, and `removeItem` on a path ending in `..` **succeeds and recursively
deletes the parent**. `Documents` is where that app keeps `Disks`, `Imports`
and `Exports`. So `W8 ANYFILE.TXT ..` destroys the user's entire disk library,
the `try?` swallows the error, and the guest is told the export succeeded.

Reproduced on a Mac with `swift`: a test tree `Documents/{keepme.txt,
Exports/a.txt}` was reduced to nothing.

## What gates it

Measured, not argued — this is what makes the ordering work.

| Question | Answer | How it was checked |
|---|---|---|
| Can the **old** W8 emit `..`? | **No.** | The CCP treats `.` as a filename delimiter, so a `.` never reaches the FCB name field. Built `98eb6a1^`, ran it: `w8 ..` prints the usage message. `cpmtools` will not create a CP/M file named `..` either ("illegal CP/M filename"). |
| Can the old W8 emit a separator? | **Yes**, `/` is not a CCP delimiter. | A CP/M file named `A/B` (cpmtools makes one) gives `To host: a/b`. On iOS that is `Exports/a/b`: `removeItem` no-ops on the absent nested path and the write fails. A failed export, not a loss — but the same string reaching the same two calls. |
| Do shipped images carry the old W8? | **Yes — verified 2026-08-26, in `ioscpm`.** | The `v1.4.5` catalog's `hd1k_combo.img` sha256 (`be19984e…`) matches no version in this repo — a separate lineage — and that used to be the whole of the evidence. `ioscpm`'s `release_assets/` copy has since been hashed against the published asset: they are byte-identical, and that image carries the OLD, non-host-path `W8`. So the exposure build 52 closes is via images a user *imports through Files*, not via anything the pinned release serves. |
| Is anyone exposed **today**? | **Yes, by user-imported images — and it is already public.** | The host-path `W8` (1408 bytes, no probe) is in `disks/hd1k_combo.img` from `98eb6a1` and `disks/hd1k_infocom.img` from `de85946`, both on **`origin/main`** since 2026-08-24 — not a local commit. `ContentView.swift`'s `fileImporter` accepts `.data`/`.item`, so a user copies one in through Files and `W8 ANYFILE.TXT ..` is live against `ioscpm` ≤ 51. Discriminator between the armed and the interlocked `W8`: `xxd -p w8.com \| tr -d '\n' \| grep -c 06e9cf` (the `ld b,0E9h` / `rst 8` probe) — 1 = interlocked, 0 = armed. The `[hostpath]` usage string does **not** distinguish them. |

## The thing that must never happen

> **Do not attach refreshed disk images to the existing `v1.4.5` tag.**

Every port pins the catalog *tag* in its binary — `ioscpm`'s
`releaseTag = "v1.4.5"` (`EmulatorViewModel.swift:128`), `z80cpmw`'s
`RELEASE_TAG = L"v1.4.5"` (`DiskCatalog.cpp:17`), and per `FEATURE_PARITY.md`
`cpmdroid`'s `DiskCatalogRepository.kt:27`. That pinning is the safety
property: new assets under a **new** tag reach nobody until a build points at
them, so publishing them arms nothing on its own. Overwriting `v1.4.5`'s assets
throws that away — every already-installed app starts fetching the new images
with no update. With the interlock in place the `W8` consequence softened from
data-loss to a broken feature (an old build gets a refusal instead of an
export), but `R8`'s host path is unprobed, so the `R8` read-side substitution
still goes live retroactively. Not an emergency for `W8` any more; still not to
be done by accident.

**And a second, artifact-level rule the tag rule does not cover:** never
publish — to *any* tag — a disk image whose `w8.com` lacks the probe. The most
dangerous artifact in this repo's history is a host-path-capable `W8` with no
interlock, and three of them exist as git blobs (`98eb6a1`..`fc68ca0`, plus the
`6e1f134` rebuild, which is no longer local — see step 0).
`disks/verify_disk_utils.sh` should assert the shipped `w8.com` contains
`06 e9 cf` so a stale one cannot be attached by mistake.

**Done 2026-08-27.** That assertion is in the script. It searches the copy
*inside each image* for the three bytes `06 e9 cf` — `ld b,H_CAPS` then `rst 8`,
which is the whole of `w8.asm`'s `check_host_path_safe` — and it is asked before
the staleness comparison rather than after, because the layout and byte checks
both bail on a mismatch and "is it stale" and "does it hand over a host path
unguarded" are different questions. Verified both ways: the two tracked images
pass, and the armed `w8.com` recovered from `de85946` fails with
`hands over a host path with no interlock`. The check runs under `make -C src
test`, so CI covers it. Nothing in the file's *text* discriminates — the armed
1408-byte build prints the same `Usage: W8 <cpmname> [hostpath]` as the
1792-byte interlocked one — which is why the assertion is on instruction bytes.

Everything below depends on both rules holding.

---

## The order

### Step 0 — this repository. **Pushed. `main` and `v1.36` are public.**

**Corrected 2026-08-27.** This section used to read "Committed locally, NOT
pushed", with `main` at `a4d3db8`, `origin/main` at `fc68ca0` and `v1.35` the
newest pushed tag. All of that is now false, and it was the premise the rest of
the file was written on, so it is the first thing to fix:

```
$ git ls-remote --tags --heads origin
a95db9f…  refs/heads/main
c5f98ba…  refs/tags/v1.36
04ad2b6…  refs/tags/v1.36^{}
```

`main` is at `a95db9f`, five commits past the tag (`17cd380`, `322ca8e`,
`657d61a`, `5920681`, `a95db9f`). *(2026-09-01: six now - `8eeb227` followed,
and `04ad2b6..8eeb227` is the code `v1.37` ships; `git log v1.36..v1.37` shows a
seventh, the release commit itself.)* The tagged commit `04ad2b6` carries the core
fixes, the shared `emu_host_path_basename()`, `HBF_HOST_GETNAME` and the
`HBF_HOST_CAPS` interlock; `322ca8e` rebuilt both tracked images again after it.

**So the claim that the public repository serves the armed host-path `W8` with
none of the protection is no longer true of `HEAD`.** Both tracked images carry
the interlocked `W8` — measured, not assumed: `disks/verify_disk_utils.sh` now
asserts the `06 e9 cf` probe on the copy inside each image and both pass. The
gate this section used to ask you to run by hand
(`xxd -p w8.com | tr -d '\n' | grep -q 06e9cf`) is that assertion; `make -C src
test` runs it.

What pushing did **not** do is remove the armed blobs. `98eb6a1`, `de85946` and
`6e1f134` are all ancestors of `main`, so all three armed images went from local
to *published* with the push, and a user who already has one of those images is
exactly where they were.

Remaining here:

- **There is no `v1.36` GitHub release, and there will not be one. Decided
  2026-09-01.** This bullet used to end "so cutting it is independent of
  everything below", which read as an invitation to cut it. Cutting it would
  fail. A `release: published` event runs the workflow file *from the tag's own
  commit*, not from `main` - measured, not assumed: the successful `v1.35`
  release run `31177155425` reports `headSha` `13d1c23`, which is exactly
  `git rev-parse v1.35^{}`. And `git show v1.36:.github/workflows/release.yml`
  pins `CPMEMU_REF: 9fee3c2`, the cpmemu commit whose `src/makefile` puts the
  Clang-only `-Wshorten-64-to-32` into `CXXFLAGS` unconditionally, so
  `make libqkz80.a` cannot build on a GitHub GCC runner. That is not a
  prediction either: the tagged `test.yml` pins the same `9fee3c2` and runs the
  same clone-and-`make libqkz80.a` step, and run `32880455802` — that workflow,
  on a push of `04ad2b6` itself — is a failure whose one failed step is named
  "Build qkz80 library". The tagged workflow also predates the `web/vendor/`
  and `roms/emu_avw.rom` staging (grep the tagged file for `vendor` or
  `emu_avw`: nothing), so a build that somehow got past the library step would
  install a page whose ROM 404s, and — since the tagged template still loads
  xterm from `cdn.jsdelivr.net` — one with no terminal on any machine without
  internet.

  So `v1.37` is cut from `main` instead. The `v1.36` tag stays exactly where it
  is - it is a published core-ABI contract that `ioscpm`, `cpmdroid` and
  `z80cpmw` coordinate against, and `DOWNSTREAM.md`'s `v1.36:` checklist items
  are measured against it - it is simply never packaged. `gh release list`
  shows `v1.35` (2026-08-07) as Latest until `v1.37` exists. The tag carries no
  disk images either way, so all of this remains independent of steps 4 and 5.
- The `README.md` line this plan asked for **is now written**, in the
  **Recommended Disk Images** section directly under the table. It is still the
  only measure that reaches someone who already has the file, since they read
  the repo rather than re-clone.

### Step 1 — `ioscpm` code fix. **Done and committed as `bb5543f`.**

**Corrected 2026-08-27.** This section used to say "Done, uncommitted". It is
committed: `bb5543f`, "Build 52: W8 could delete the user's disk library". That
port has since moved on to `15f48e9` and has picked up the two core-sync
obligations as well — `49851aa` defines `emu_host_path_caps()` and `15f48e9`
defines `emu_host_file_get_read_name()`.

All three changes are made and verified in `/Users/wohl/src/ioscpm`: 48
host-side checks pass, the app builds, and `W8 ANYFILE.TXT ..` now leaves
`Documents` intact where the shipped logic reported `Documents: GONE`. A new
`ExportPath` type owns the reduction and the containment check so both are
testable (`Tests/ExportPathTests.swift`, 24 checks). `releaseTag` is
deliberately untouched at `v1.4.5`. What follows is what was done:

1. **`emu_io_ios.mm`, `emu_host_file_open_write()`** — reduce the incoming
   string with `emu_host_path_basename()`. That port symlinks
   `emu_io_common.cc` into `iOSCPM/Core/`, so the function is there on the next
   build with nothing to add. *This one change closes the destructive case*,
   and closes it for any future UI layer too.
2. **`EmulatorViewModel.swift`, `saveToExportsFolder()`** — assert
   `destURL.standardized.path` still has `exportsDir.standardized.path` as a
   prefix before writing, and drop the `removeItem` entirely
   (`Data.write(to:)` already replaces). Defence in depth behind (1).
3. **`EmulatorViewModel.swift:2643-2659`, the R8 side** — reduce to a leaf
   name, look *that* up in `Imports`, and report a miss with
   `emu_host_file_cancel()` instead of falling back to the first file in the
   folder. Not destructive, but it silently imports unrelated contents under
   the requested name, and it needs **no image change to be live today**.

Then, separately and worth doing at the same time: make
`emu_host_file_get_write_name()` return the Exports location the Swift layer
will really write, so `W8` can tell the user where the file went.

**Remaining: ship it as a build users have.** Do not bump `releaseTag` in this
build unless the new assets already exist.

### Step 2 — `cpmdroid` code fix. **Code done.** Blocks step 5 for that port only.

**Corrected 2026-08-27.** This section used to say "Not checked out on this
machine". It is: `/Users/wohl/src/cpmdroid`, at `c6756af`. The task it set —
confirm the port has the same shape and make the same three changes — is done,
in `a523d40` ("Sync to the romwbw_emu v1.36 core, and close four keyboard and
file gaps"). In `app/src/main/cpp/emu_io_android.cpp` today:

- `emu_host_path_basename()` is there as its own copy (CMakeLists does not
  build `emu_io_common.cc`, the same constraint `z80cpmw` has),
- `android_host_leaf()` reduces a guest-supplied name to a leaf, and **both**
  `emu_host_file_open_read()` and `emu_host_file_open_write()` go through it,
- `emu_host_path_caps()` is defined, which is what lets the port link the
  `v1.36` core at all,
- and `emu_host_file_get_read_name()` is defined too, so the post-`v1.36` core
  links there as well.

**Heading corrected 2026-09-01.** It said "Code half done." while the four
bullets under it list the code as complete, which is the sort of contradiction a
port reads the heading of and not the body. Re-checked against
`/home/wohl/src/cpmdroid` at `22f10df`, tree clean and in sync with its
upstream: `emu_host_path_caps()` is at `app/src/main/cpp/emu_io_android.cpp:753`
and `emu_host_file_get_read_name()` at `:898`, both present since `a523d40` and
so already true when the body was written. `167acbe` (2026-08-29) then made the
read-name answer a gated one — it returns `""` unless the state is
`HOST_FILE_READING`, because on this port an open only parks the request for the
Kotlin layer to poll, so at `0xEA` time there is usually nothing honest to say
yet and R8 falls back to printing what was typed.

What is left for this port is step 5's half, not the code: it must not bump its
catalog pin before a build carrying the above is what users have — and see the
catalog-`version` hazard below, which applies to it as much as to `ioscpm` if it
implements the same invalidation.

### Step 3 — `z80cpmw` code fix. **Code done.** Not a safety blocker.

No delete was found there: that backend is `fopen`-based, honours absolute
paths deliberately, and has no `removeItem`. Its change is about *reporting* —
return `resolveHostPath()` + the `resolveRealPath()` it already has from
`emu_host_file_get_write_name()`, so an installed MSIX build tells the user the
redirected `LocalCache` path instead of one the OS silently moved.

Note it **cannot** just compile the shared helper: its vcxproj takes only
`hbios_dispatch.cc`, `hbios_cpu.cc`, `emu_init.cc` and headers from this tree,
and adding `emu_io_common.cc` would collide on ten symbols
`emu_io_windows.cpp` already defines. Copy the ~25-line function.

**2026-08-27: the half named above is done, and the port no longer links.** The
reporting change landed in `z80cpmw` `2f10d4c` ("Sync to the romwbw_emu v1.36
core, and tell W8 where the file really went"), and
`emu_host_file_get_write_name()` and `emu_host_path_caps()` are both defined in
`z80cpmw/emu_io_windows.cpp`. `emu_host_file_get_read_name()` is **not** —
checked against that repo's committed `HEAD`, `f197dde`. The core calls it
unconditionally as of `322ca8e` (`hbios_dispatch.cc`, `HBF_HOST_GETRNAME`), so
syncing anything past `v1.36` into that vcxproj fails to link, exactly the way
`emu_host_path_caps()` did before it. `return "";` is a correct answer and costs
nothing — see this repo's `docs/DOWNSTREAM_2026-08-26.md`. Add it with the sync,
not after it.

**Corrected 2026-09-01: the port links. The paragraph above was true for one
day.** `f197dde` is 2026-08-26; `713bfce` ("Define the read-name getter the core
requires, and check disk assets before a release") is 2026-08-27 and defines
`emu_host_file_get_read_name()` at `z80cpmw/emu_io_windows.cpp:1067`, with
`emu_host_path_caps()` beside it at `:1083`. Re-checked on this machine against
`/home/wohl/src/z80cpmw` at `148be4f`, tree clean and in sync with
`origin/master`, and `f197dde` is an ancestor of it — so the "checked against
`f197dde`" reading was correct when written and is simply out of date, not
wrong. Anyone syncing this repo's `v1.37` sources into that vcxproj links.

It did not take the free `return "";`, which is worth recording because it is
the case the contract was written for: this backend resolved the guest's name to
a real path in `emu_host_file_open_read()`, so it returns that path, and an
installed MSIX build tells the user the redirected
`...\Packages\...\LocalCache\Local` location instead of the bare name they
typed. It answers `""` only when the bytes came from
`emu_host_file_provide_data()`, which has no path to report.

Can happen before or after step 4; it is independent.

### Step 4 — publish refreshed disk images under a **new** `ioscpm` tag.

Still after step 1 — but now because an unfixed port would *refuse* the feature
rather than destroy the user's data, which is a broken feature rather than an
emergency. Publishing before step 1 lands is no longer unsafe; it is just bad. `disks/rebuild_disk_utils.sh` builds and
installs; `disks/verify_disk_utils.sh` checks. New tag, new assets, `v1.4.5`
untouched.

**2026-08-27: "refreshed images" now means `HEAD`'s generation, not `v1.36`'s.**
The images moved again after the tag — `322ca8e` rebuilt both — and `5920681`
rewrote `disks/diskdefs`, which is what the rebuild and verify scripts read to
find anything inside them. Publish from `main`, not from the tag, and run
`disks/verify_disk_utils.sh` on exactly the files being uploaded: it now asserts
both that each `r8.com`/`w8.com` matches its source and that the `w8.com` on the
image carries the `HBF_HOST_CAPS` probe.

**And this step is what fires the second data-loss path.** Read the next section
before bumping the catalog's `version` attribute.

### Step 5 — bump each port's catalog pin, per port, in a build that already has that port's fix.

`ioscpm` `releaseTag`, `z80cpmw` `RELEASE_TAG`, `cpmdroid` `RELEASE_TAG`. This
is the moment the new W8 reaches ordinary users of each port, and the rule is
one line:

> **Never ship a build that bumps the catalog pin without that port's
> sanitiser in the same binary.**

### Step 5, second hazard — bumping the catalog `version` deletes the user's disks

**Added 2026-08-27.** This document reasoned only about `W8` and `R8`. There is
a second, unrelated data-loss path on the *same* release step, and step 4's own
instructions are what arm it. The full write-up is `ioscpm`'s `todo.txt` entry
"THE SECOND DATA-LOSS PATH ON THAT SAME RELEASE STEP" (written 2026-08-26);
this is the part that belongs in the ordering.

`disks.xml` carries a `version` attribute. On every successful catalog fetch,
`checkCatalogVersionAndInvalidate()` (`EmulatorViewModel.swift`) compares it
against the stored `catalogVersion` default and, on **any** difference, calls
`deleteAllDownloadedDisks()` and then tells the user it has happened. That
function loops over `contents where url.pathExtension == "img"` in
`Documents/Disks` and removes every one — so it also deletes:

- disks the user imported through Files, and
- disks the app itself created with `createNewDisk`.

Neither is in the catalog, so neither can be re-downloaded. Those are gone.
A downloaded disk is writable and is exactly where a user's CP/M work lives, so
even the catalog-restorable ones lose everything written since the download.

Three things make this an ordering problem rather than a footnote:

- **Step 4 tells you to bump the attribute.** It is at `13` today, both in
  `ioscpm`'s `release_assets/` and on the pinned `v1.4.5` release (checked
  2026-08-26 — they agree byte for byte). It has moved on essentially every
  catalog change. Publishing refreshed images without touching it is the only
  way to avoid firing this, and that is not what the runbooks say to do.
- **It needs no download and no tap.** The wipe happens on the next launch that
  fetches a catalog, on every installed device at once.
- **It has already fired once, in the field.** The `12` → `13` bump reached
  users on 2026-07-22, when `disks.xml` was uploaded to `v1.4.11` (asset
  timestamp `2026-07-22T10:41:09Z`). The app still floated on `releases/latest`
  then — the pin landed three days later — so every installed client holding
  `12` fetched `13` and wiped its `Disks` folder. Nothing recorded it at the
  time; the alert is all the user got.

There is no patch to apply here, because the fix is a product decision:
copy-on-write on first guest write, or confirm before wiping, or spare anything
the catalog does not name, or keep the wipe and give the user an export path
first. Whichever it is, **it has to be in the same binary as the catalog bump**,
for exactly the reason build 52 has to be. Add it to the step-5 rule:

> **Never ship a build that bumps the catalog pin — or publish a catalog whose
> `version` attribute moved — without both that port's sanitiser and its answer
> to the invalidation wipe in the same binary.**

`cpmdroid` and `z80cpmw` should each be checked for the same invalidation
behaviour before their own step 5; this was measured in `ioscpm` only.

---

## Why this order and not the reverse

There is a real tension, and it is worth stating rather than glossing.

The **old** R8 that shipped images carry has its own destructive bug: an
unfiltered host basename makes an ambiguous FCB, which `F_DELETE` then uses to
erase **every matching CP/M file** before creating the new one. Importing a
host file called `a?b.txt` deletes everything matching `A?B.TXT` and says
nothing — verified live, two unrelated files went with it. That is live today
on every port, and refreshing the images is what fixes it.

So refreshing sooner fixes one destructive bug and arms another. The order
above is chosen on blast radius:

- Old R8 destroys files **inside the CP/M disk image**, and needs a host file
  whose name contains `?` or `*`.
- New W8 on an unfixed `ioscpm` destroys the **host Documents folder** —
  including every disk image the user has downloaded, which is where their CP/M
  data lives in the first place.

The second is strictly worse and takes the first with it. Fix the ports, then
refresh.

## Quick check before each of steps 4 and 5

1. Does the build being shipped contain that port's sanitiser? If no, stop.
2. Are the new images under a **new** tag, with `v1.4.5` untouched? If no, stop.
3. Does `disks/verify_disk_utils.sh` pass on the images being published? It
   answers two questions now, not one: that each `r8.com`/`w8.com` matches its
   source, and that the `w8.com` on the image carries the `HBF_HOST_CAPS` probe.
4. Did the catalog's `version` attribute move? If it did, does the build being
   shipped answer the invalidation wipe? If no, stop — see "Step 5, second
   hazard" above.
5. In the shipped build, does `W8 ANYFILE.TXT ..` leave `Documents` intact?
   Necessary but not sufficient: the interlock passes this test on an
   unsanitised port too, because the emulator refuses the path. The test that
   actually exercises the backend is a crafted `.COM` that calls HBIOS `0xE2`
   with `..` directly (it bypasses `W8` entirely) — that must ALSO leave
   `Documents` intact, and only the backend sanitiser makes it so. Do both by
   hand, on a device, with a throwaway disk library.

---

## Design note: make the capability impossible to assert by accident

`HBF_HOST_CAPS` currently returns `HOST_CAP_SAFE_PATHS` from shared core code
with no backend involvement (`hbios_dispatch.cc`), so compiling the v1.36 core
*is* the assertion — a port that has not sanitised still claims it has, and
`W8` believes it. Two known-false instances today: the CLI and Windows backends
both set the bit and both deliberately honour arbitrary absolute paths, so the
bit's documented contract ("a host path cannot escape where the front end
writes") is literally untrue there.

**This is fixed as of v1.36's `a4d3db8`+ (the `emu_host_path_caps()` refactor).**
`HBF_HOST_CAPS` now returns the value of `emu_host_path_caps()`, a backend
function `emu_io.h` declares but the core does not define - so a port that syncs
the core without supplying it fails to *link* rather than silently asserting a
guarantee its code does not make. The guarantee and the code that makes it true
now arrive together by construction. The CLI and browser backends here define it
(both set the bit); `z80cpmw` and `cpmdroid` must add it before they can build
against the core, which is the point. *(2026-08-27: all three ports have added
it — `ioscpm` in `49851aa`, `cpmdroid` in `a523d40`, `z80cpmw` in `2f10d4c`. The
same construction has since caught a second one:
`emu_host_file_get_read_name()` is declared and not defined, and `z80cpmw` has
not added it yet. See step 3.)* *(2026-09-01: `z80cpmw` added it in `713bfce`,
so the second catch is closed too and all three ports define both. That is two
for two: each time the core has taken a guarantee away from a constant and given
it to a backend function, the linker has named every port that had to answer,
and none of them found out from prose.)* The bit's meaning was also corrected: it
is "the path is never used destructively", not "confined to one directory", so a
backend that honours absolute paths sets it honestly.
