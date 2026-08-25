# Release order for the W8 host-path work

**The constraint this whole document exists for:** refreshing the published
disk images is what arms a data-destruction bug in `ioscpm`. The images and the
port fixes cannot go out in either order — the fixes go first.

This is the coordination plan across four repositories. The technical detail is
in [DOWNSTREAM_2026-08-25.md](DOWNSTREAM_2026-08-25.md); this file is only the
ordering, and why it is that order.

> ### Update: the order is no longer the only thing standing between a user and the bug
>
> An ordering plan only governs the channel it controls, and this one controls
> the catalog. It never covered an image travelling any other way — someone
> copies one in by hand, takes one from a friend, or downloads
> `romwbw_emu/disks/`. That gap was live and this document could not close it.
>
> `W8.COM` now closes it itself. It asks the emulator, before opening anything,
> whether a host path is safe to send (`HBF_HOST_CAPS`, 0xE9 — a call no older
> emulator has). If the answer is no, it refuses and writes nothing. So a
> refreshed image meeting an unfixed front end is now a **refusal**, not a
> deletion, whatever route the image took.
>
> Two things this does **not** change:
>
> - **Step 1 is still first, and still urgent.** The interlock makes the
>   dangerous combination fail safe; it does not fix the port. Until step 1
>   ships, `ioscpm` users get a refusal where they should get an export, and any
>   image built between `98eb6a1` and the interlock still carries a `W8` that
>   asks for nothing and gets no protection.
> - **The `v1.4.5` rule below is unchanged.** Overwriting those assets would
>   still push new images to every installed app with no update. Safe now rather
>   than destructive — but still not something to do by accident.
>
> And it adds one obligation, in `DOWNSTREAM_2026-08-25.md` section 1b:
> compiling the v1.36 core is what asserts the safety bit `W8` trusts. A port
> that takes the core without sanitising sets the bit, `W8` believes it, and the
> interlock is bypassed. **`cpmdroid` must not take the core without step 2.**

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
| Is anyone exposed **today**? | **Yes, by user-imported images.** | `ContentView.swift`'s `fileImporter` accepts `.data`/`.item`, and this repo's `disks/hd1k_combo.img` carries the new W8 as of `6e1f134`. Copy it in through Files and `W8 ANYFILE.TXT ..` is live. |

## The thing that must never happen

> **Do not attach refreshed disk images to the existing `v1.4.5` tag.**

Every port pins the catalog *tag* in its binary — `ioscpm`'s
`releaseTag = "v1.4.5"` (`EmulatorViewModel.swift:128`), `z80cpmw`'s
`RELEASE_TAG = L"v1.4.5"` (`DiskCatalog.cpp:17`), and per `FEATURE_PARITY.md`
`cpmdroid`'s `DiskCatalogRepository.kt:27`. That pinning is the safety
property: new assets under a **new** tag reach nobody until a build points at
them, so publishing them arms nothing on its own. Overwriting `v1.4.5`'s assets
throws that away — every already-installed app starts fetching the new images
with no update, and the hazard goes live retroactively on every unfixed port at
once.

Everything below depends on that one rule holding.

---

## The order

### Step 0 — this repository. Done.

`6e1f134`, `a0c3099`, `23b066f`, tagged `v1.36`. Core fixes, the shared
`emu_host_path_basename()`, `HBF_HOST_GETNAME`, and both tracked images
rebuilt.

Two consequences to be aware of rather than act on:

- The `v1.36` **tag carries no disk images** — `release.yml` stages the binary,
  the web page and `roms/` only. Cutting the GitHub release is what builds the
  wasm, so it is how the browser-side fixes reach anyone; it is independent of
  everything below and can happen at any time.
- This repo's `disks/*.img` are now the live source of the user-import exposure
  above. If step 1 will take a while, consider a line in `README.md` next to
  the disk images saying not to load them into `ioscpm` until its next build.

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
   That is the one manual test worth doing by hand, on a device, with a
   throwaway disk library.
