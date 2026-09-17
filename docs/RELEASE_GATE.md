# The release gate, and why it went

`ROMWBW_SUPPORTED_RELEASES` in `src/romwbw_pin.h` gated ROM loading on the
RomWBW release number. It gated the wrong axis, duplicated a mechanism that
already existed one repository over, and was the single reason publishing a new
RomWBW release required releasing Windows, macOS, iOS, Android and Linux at
once — the exact coupling `romwbw_disks` was built to remove.

**It was removed in v1.44, along with `src/romwbw_pin.h` itself.** This note
was written on 2026-09-17 arguing that it should go; what follows is the record
of what went, what stayed, and what downstream has to do. The argument is kept
because it is the reason, and because the same mistake is easy to re-introduce.

## Two axes, and only one of them is ours

**The RomWBW release** — 3.5.1, 3.6.0, a future 3.7.0 — is a pairing between
HBIOS and the CBIOS in a disk image's boot slice. Mismatch them and the guest
prints `*** WARNING: HBIOS/CBIOS Version Mismatch ***`. That warning comes from
the guest, not from us. It is a fact about a ROM and a disk image, and the
catalog already enforces it by naming both versions in every filename.

**The emulator-to-ROM interface** is what this core actually depends on, and it
is three things:

- two I/O ports, `EMU_DISPATCH_PORT equ 0EFh` and `EMU_BNKCALL_PORT equ 0EDh`
  (`src/emu_hbios.asm`);
- the set of HBIOS functions the C++ dispatcher services (`src/hbios_dispatch.cc`);
- one hardcoded guest address, `0x0406`, special-cased in `HBX_BNKCALL`
  (`src/emu_hbios.asm`) and in `handlePRTSUM` (`src/hbios_cpu.cc`).

None of those three is versioned by a release number. The thing that *was*
versioned was the release number, which is the other axis.

## The interface version already existed

`romwbw_disks/docs/INTERFACE_V0.md` defines `v0` as the contract between that
repository and every client, and its third pillar is the HBIOS host-extension
ABI — the `0xE1`–`0xEA` private block plus the two standard functions the
dispatcher handles on the same path. Its bump rules name a breaking change to
that ABI as a reason to go to `v1`. Its "What v0 does not cover" section says:

> **The RomWBW version.** That is data in the catalog, not part of the
> contract. Adding RomWBW 3.7.0 is a new release tag and a regenerated index —
> no client change, no interface bump.

**That sentence was false, and this core was the reason.** A regenerated index
carrying 3.7.0 would be fetched, parsed, and then discarded by every client,
because each asked `emu_romwbw_release_supported()` per entry and the answer
came from a two-line macro compiled into the binary. Even with the client
filters deleted, `emu_validate_rom_hcb` refused the ROM at load. It is true
now: a ROM whose HCB reads `37 00` loads and boots, which was checked against a
synthesised image before this was written.

The migration mechanism was already built and already shipped. Every client
compiles in `releases/latest/download/index-v0.json`, which names no tag, so a
`v1` ships as `index-v1.json` beside `index-v0.json` on the release marked
Latest: v0 clients keep reading v0, v1 clients read v1, and neither is rebuilt
for the other's sake. That is a per-generation gate, owned entirely by
`romwbw_disks`, that costs no application build. The release allowlist was a
second, finer, weaker gate solving the same problem at the price of five app
releases.

## The core refused ROMs on a number it did not own

`romwbw_disks/tools/build_rom.sh` generates `romwbw_ver.inc` from
`versions/<ver>/version.json` and stamps the HCB at `0x105`/`0x106`. So the
release bytes in a published `emu_*.rom` are `romwbw_disks` build data. This
core then read them back and refused the ROM if they were not in a list
maintained by hand here. The number was written there and adjudicated here, and
the two places had no way to stay in step except a person editing a macro.

## What the allowlist was standing in for, and what replaced it

v0's third pillar covers the private `0xE1`–`0xEA` block and two standard
functions. It does **not** enumerate the standard RomWBW HBIOS functions
`hbios_dispatch.cc` implements. A future release whose CBIOS calls a standard
function this dispatcher lacks would not be caught by v0 as written, and that
is the real risk the release allowlist was standing in for.

`romwbw_disks/tools/boot_test.sh` is what replaced it. It asserts the CBIOS
banner, the CP/M prompt, no mismatch warning on a matched pair, a warning on a
mismatched one, and an R8/W8 round trip. It is a measurement of the actual pair
rather than a list someone remembered to edit, it runs against the artifact
being published, and it runs at the moment the decision is actually made.

The honest trade: an entry in `ROMWBW_SUPPORTED_RELEASES` meant "somebody
booted *this binary* against that release." A passing `boot_test.sh` means
"`romwbw_disks` booted *a* build of the proxy against that release before
publishing it." That is weaker in principle and stronger in practice, because
the compile-time list was edited months before the release it blessed and was
never re-checked, whereas `boot_test.sh` runs every time.

### `boot_test.sh` consumed the gate, so it could not be left alone

It parsed `RomWBW releases this build can run:` off `romwbw_emu --version` to
build `RUNS`, and `die`d — exit 1, not a skip — when that line was absent. Cut
the gate without touching it and the publish-time gate would not have lost a
fact; it would have **failed**, blocking every publish.

That is why it changed first. It now derives the versions to test from
`versions/` unconditionally, parses no banner, and has no refusal branch: a ROM
that does not boot is a failure, never a correct refusal. Publishing into the
v0 index is the assertion that it passed.

## `0x0406` is not release-sensitive after all

This note used to call `0x0406` "the one genuine release constant in bank 0"
and propose moving it into the generated `romwbw_ver.inc`. That was wrong, and
checking it is what showed why: upstream 3.6.0's `romldr.asm` contains **no
reference to `$0406` at all**. The special case is live under 3.5.1 and inert
under 3.6.0. Parameterising it would buy nothing and would move all four
published ROMs' hashes, bumping both catalog generations and making every user
re-download their disk images.

It also has a second half nothing had named: `handlePRTSUM` in
`src/hbios_cpu.cc` is C++ compiled into every client, which `romwbw_ver.inc`
could never have reached.

Left alone, deliberately.

## What came out

Here, in v1.44:

- `src/romwbw_pin.h` — the whole file, `ROMWBW_SUPPORTED_RELEASES` and
  `ROMWBW_DEFAULT_*` both
- the release branch of `emu_validate_rom_hcb` (`src/emu_init.cc`); the
  size check, the HCB marker check and the `CB_PLATFORM` warning stay
- `emu_romwbw_release_supported()`, `emu_romwbw_supported_list()`,
  `emu_set_allow_untested_romwbw()`, `emu_allow_untested_romwbw()` and the
  supported-release table they read
- the `RomWBW releases this build can run:` line in `print_version_banner()`.
  That line was a cross-repository protocol with four consumers:
  `tools/romwbw-get`'s `RE_RUNS`, `roms/verify_romwbw_pin.sh`, `romwbw_disks`'
  `tools/boot_test.sh`, and the `romwbw_supported_releases` wasm export
- `--allow-untested-romwbw` and its usage text (`src/romwbw_emu.cc`)
- the gate half of `roms/verify_romwbw_pin.sh`. **The script stays**, with its
  name and path: its HCB-marker, `CB_PLATFORM` and ROM↔disk **pairing** checks
  are release-agnostic, and the pairing check is the only thing in this
  repository that enforces the axis that is real. Its two PASS lines changed
  spelling, and `.github/workflows/test.yml` matches the new ones
- in `tools/romwbw-get`: `supported_releases()`, `RE_RUNS`/`RE_PINNED`,
  `_runnable()` and its eight call sites, the `runnable` parameter of
  `Catalog.pool()` and `resolve_version()`, the no-overlap `Contradiction`,
  `cmd_use`'s refusal, `cmd_run`'s `--allow-untested-romwbw` passthrough, and
  the `emu_supported` key in both `versions --json` and the mirror manifest.
  `--assume-supported`, `--allow-untested` and `versions --all` **stay as
  accepted no-ops**, and `mirror --versions=runnable` still parses and means
  `all`, because installed scripts and test suites pass them and an argparse
  error would turn "this flag is obsolete" into exit 64
- in `web/`: `romwbw_supported_releases()` (`romwbw_web.cc`), its
  `EXPORTED_FUNCTIONS` entry, and in `romwbw.html-template` `versionRunnable`,
  `applyCoreSupported`, `coreSupportedReleases`, the `disabled` /
  "(needs a newer build)" logic in `fillVersionSelect`, and the
  `onRuntimeInitialized` ask-the-core block. `make -C web check` **stays** —
  it also runs `tests/web_nvram_boot.js`, which has nothing to do with this
- `tests/web_supported_releases.js` (deleted), the gate sections and lifted
  functions of `tests/web_manifest.js`, and the gate sections and
  `--assume-supported` of `tests/catalog_client_test.py`

`emu_validate_rom_hcb()` keeps its name and signature. So do
`emu_romwbw_release_of_image()`, `emu_romwbw_release_loaded()`,
`emu_romwbw_release_str()`, the `emu_romwbw_release` struct and
`emu_load_rom_from_buffer()`.

### The same change took the last release number out of the tree

Removing the gate did not by itself meet "this repository should not know that
3.5.1 and 3.6.0 exist". Two more things did:

- `src/emu_hbios.asm` hardcoded `db 035h` / `db 010h` at `CB_VERSION` and in
  the proxy ident block. It now takes both from a generated `romwbw_ver.inc`,
  and is **byte-identical** to the copy in `romwbw_disks`. That moved no
  published byte — published ROMs are built from that repository's copy, which
  was already parameterised — and `check_source_drift.sh` there changed from
  asserting the two copies *differ* in a documented way to asserting they are
  *identical*, which is strictly stronger.
- `roms/build_emu_rom.sh` read `ROMWBW_DEFAULT_*` with `sed` and could build
  only 3.5.1 — by then not even the catalog's default, which is 3.6.0. It now
  takes `--romwbw VER`, defaults to the catalog's default, and generates the
  version stamp from the HCB of the stock ROM it is overlaying, so bank 0 and
  banks 1–15 name the same release by construction. All four published ROMs
  (`emu_avw` and `emu_rcz80` × 3.5.1 and 3.6.0) reproduce byte-for-byte.

## What downstream must do

**All three GUI clients break at compile time on the same commit**, and this
note previously said only `ioscpm` did. That was wrong: `ioscpm` symlinks ten
files out of `src/` including `emu_init.cc` and `romwbw_pin.h`, and `z80cpmw`
and `cpmdroid` compile `romwbw_emu/src/emu_init.cc` **in place** out of a
sibling checkout. The deleted functions are called directly by all three.

- `z80cpmw` — `catalogv0::runnableVersions` (`CatalogV0.cpp`), the predicate
  and empty-set error and filtered copy (`DiskCatalog.cpp`), the About box
  (`MainWindow.cpp`), the `<ClInclude>` of `romwbw_pin.h` in `z80cpmw.vcxproj`,
  and `tests/test_catalogv0.cpp`
- `ioscpm` — `RomWBWIndex.offered` (`CatalogDocument.swift`), both bridge
  methods `+supportsRomWBWVer:upd:` and `+romWBWReleases` (`RomWBWEmulator.mm`),
  the `noSupportedRelease` `CatalogFailure` stage, the `todaysCore` stub in
  `CatalogDocumentTests`, `check_view_bindings.sh`, and the now-dangling
  `iOSCPM/Core/romwbw_pin.h` symlink
- `cpmdroid` — `runnableRomwbwVersions` (`RomwbwIndex.kt`), both JNI exports
  (`emu_io_android.cpp`), the `external fun` declarations in
  `EmulatorEngine.kt`, `RomwbwSupport.kt` and its callers in
  `CatalogLoader.kt`, `SettingsActivity.kt` and `MainActivity.kt`, and
  `JniNameParityTest.kt`, which asserts by name that both natives are bound on
  both sides

`cpmemu` is clean — grep for all four symbols returns nothing.

In `romwbw_disks`: `INTERFACE_V0.md`'s "The one thing v0 could not fix on its
own" section, `CATALOG_SCHEMA.md`, `CLIENT_MIGRATION.md`, `ROMWBW_VERSIONS.md`,
`RELEASING.md` and `README.md` all describe the gate and need updating.
`boot_test.sh` and `check_source_drift.sh` were changed with this commit.

## What stays

`hbios.ver_byte` and `upd_byte` stay in every index entry. They stop being an
emulator gate and remain what they always described: the ROM-to-disk-image
pairing, which is the HBIOS/CBIOS axis and is real. A client may still use them
to refuse to mount a 3.5.1 image under a 3.6.0 ROM.

## Cost, stated plainly

- An unchecked ROM now loads and runs. The protection moved from compile time
  to publish time, and `boot_test.sh` is where it landed.
- Every client built before 2026-09-10 compiles in the `catalog-v0` tag by
  name. Those tags stay live forever regardless of anything here.
- GitHub's Latest flag is one flag for the whole repository, and
  `gh release create` claims it by default. The catalog entry point depends on
  it. `CATALOG_SCHEMA.md` §6.2 has the invariant.
