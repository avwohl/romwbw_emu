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
>   interlock. The images on `origin/main` (commits `98eb6a1`..`fc68ca0`) carry
>   a host-path-capable `W8` with no probe, public since 2026-08-24. Upgrading
>   the emulator does nothing for a user who already has one of those images.
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
| Do shipped images carry the old W8? | **Believed yes, not verified here.** | The `v1.4.5` catalog's `hd1k_combo.img` sha256 (`be19984e…`) matches no version in this repo — a separate lineage. The evidence is `z80cpmw`'s measurement of its bundled copy, which came down from that release, holding `Usage: W8 <cpmname>`. **Verify before relying on it** (see step 1). |
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
local `6e1f134` rebuild). `disks/verify_disk_utils.sh` should assert the shipped
`w8.com` contains `06 e9 cf` so a stale one cannot be attached by mistake.

Everything below depends on both rules holding.

---

## The order

### Step 0 — this repository. **Committed locally, NOT pushed.**

`main` is at `a4d3db8`, tagged `v1.36`: core fixes, the shared
`emu_host_path_basename()`, `HBF_HOST_GETNAME`, the `HBF_HOST_CAPS` interlock,
and both tracked images rebuilt with the interlocked `W8`.

**None of it is on `origin` yet.** `origin/main` is `fc68ca0` and the newest
pushed tag is `v1.35`. So today the public repository is in the worst state on
offer: it serves the armed host-path `W8` (from `98eb6a1`, above) with none of
the protection, and has since 2026-08-24. Every later step's reasoning assumes
step 0 is public; until it is pushed, it is not.

Consequences to act on:

- **Push `main` through `a4d3db8` and the `v1.36` tag.** This does nothing for
  existing clones (git keeps the armed blobs forever) but it stops *new* clones
  arming, and it makes the rest of this plan true. Before pushing or rebuilding
  an image, gate on the probe: `xxd -p w8.com | tr -d '\n' | grep -q 06e9cf`.
  **Do not push a partial series** — `6e1f134` rebuilt the images *before* the
  interlock existed, so publishing up to there would ship a third armed-but-
  unprobed image rather than closing anything.
- Add the `README.md` line the plan has always suggested and this repo still
  lacks: next to the disk-image table, that these images hand a host path to the
  emulator and an `ioscpm` build before 52 must not be given one. That is the
  only measure that reaches someone who already has the file, since they read
  the repo rather than re-clone.
- The `v1.36` **tag carries no disk images** — `release.yml` stages the binary,
  the web page and `roms/` only. Cutting the GitHub release builds the wasm and
  is independent of everything below.

### Step 1 — `ioscpm` code fix. **Done, uncommitted, build 52.**

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

### Step 2 — `cpmdroid` code fix. Blocks step 5 for that port only.

Not checked out on this machine, so the first task is to confirm it has the
same shape: Kotlin's `File(dir, name)` has the same traversal property as
`appendingPathComponent`. If it does, it needs the same three changes. Check
whether it builds `emu_io_common.cc` — if not, see the `z80cpmw` note below.

### Step 3 — `z80cpmw` code fix. Not a safety blocker.

No delete was found there: that backend is `fopen`-based, honours absolute
paths deliberately, and has no `removeItem`. Its change is about *reporting* —
return `resolveHostPath()` + the `resolveRealPath()` it already has from
`emu_host_file_get_write_name()`, so an installed MSIX build tells the user the
redirected `LocalCache` path instead of one the OS silently moved.

Note it **cannot** just compile the shared helper: its vcxproj takes only
`hbios_dispatch.cc`, `hbios_cpu.cc`, `emu_init.cc` and headers from this tree,
and adding `emu_io_common.cc` would collide on ten symbols
`emu_io_windows.cpp` already defines. Copy the ~25-line function.

Can happen before or after step 4; it is independent.

### Step 4 — publish refreshed disk images under a **new** `ioscpm` tag.

Still after step 1 — but now because an unfixed port would *refuse* the feature
rather than destroy the user's data, which is a broken feature rather than an
emergency. Publishing before step 1 lands is no longer unsafe; it is just bad. `disks/rebuild_disk_utils.sh` builds and
installs; `disks/verify_disk_utils.sh` checks. New tag, new assets, `v1.4.5`
untouched.

### Step 5 — bump each port's catalog pin, per port, in a build that already has that port's fix.

`ioscpm` `releaseTag`, `z80cpmw` `RELEASE_TAG`, `cpmdroid` `RELEASE_TAG`. This
is the moment the new W8 reaches ordinary users of each port, and the rule is
one line:

> **Never ship a build that bumps the catalog pin without that port's
> sanitiser in the same binary.**

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
3. Does `disks/verify_disk_utils.sh` pass on the images being published?
4. In the shipped build, does `W8 ANYFILE.TXT ..` leave `Documents` intact?
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
against the core, which is the point. The bit's meaning was also corrected: it
is "the path is never used destructively", not "confined to one directory", so a
backend that honours absolute paths sets it honestly.
