# The release gate, and why it should go

`ROMWBW_SUPPORTED_RELEASES` in `src/romwbw_pin.h` gates ROM loading on the
RomWBW release number. This note argues it gates the wrong axis, duplicates a
mechanism that already exists one repository over, and is the single reason
publishing a new RomWBW release requires releasing Windows, macOS, iOS, Android
and Linux at once — the exact coupling `romwbw_disks` was built to remove.

Written 2026-09-17. Nothing here has been implemented.

## Two axes, and only one of them is ours

**The RomWBW release** — 3.5.1, 3.6.0, a future 3.7.0 — is a pairing between
HBIOS and the CBIOS in a disk image's boot slice. Mismatch them and the guest
prints `*** WARNING: HBIOS/CBIOS Version Mismatch ***`. That warning comes from
the guest, not from us. It is a fact about a ROM and a disk image, and the
catalog already enforces it by naming both versions in every filename.

**The emulator-to-ROM interface** is what this core actually depends on, and it
is three things:

- two I/O ports, `EMU_DISPATCH_PORT equ 0EFh` and `EMU_BNKCALL_PORT equ 0EDh`
  (`src/emu_hbios.asm:38-39`);
- the set of HBIOS functions the C++ dispatcher services (`src/hbios_dispatch.cc`);
- one hardcoded guest address, `0x0406`, special-cased in `HBX_BNKCALL`
  (`src/emu_hbios.asm:336-365`).

None of those three is versioned here. The thing that *is* versioned is the
release number, which is the other axis.

## The interface version already exists

`romwbw_disks/docs/INTERFACE_V0.md` defines `v0` as the contract between that
repository and every client, and its third pillar is the HBIOS host-extension
ABI — the `0xE1`–`0xEA` private block plus the two standard functions the
dispatcher handles on the same path. Its bump rules name a breaking change to
that ABI as a reason to go to `v1`. Its "What v0 does not cover" section is
unambiguous:

> **The RomWBW version.** That is data in the catalog, not part of the
> contract. Adding RomWBW 3.7.0 is a new release tag and a regenerated index —
> no client change, no interface bump.

**That sentence is false today, and this core is the reason.** A regenerated
index carrying 3.7.0 would be fetched, parsed, and then discarded by every
client, because each asks `emu_romwbw_release_supported()` per entry and the
answer comes from a two-line macro compiled into the binary. Even with the
client filters deleted, `emu_validate_rom_hcb` (`src/emu_init.cc:194-211`)
refuses the ROM at load.

The migration mechanism is already built and already shipped. Every client
compiles in `releases/latest/download/index-v0.json`, which names no tag, so a
`v1` ships as `index-v1.json` beside `index-v0.json` on the release marked
Latest: v0 clients keep reading v0, v1 clients read v1, and neither is rebuilt
for the other's sake. That is a per-generation gate, owned entirely by
`romwbw_disks`, that costs no application build. The release allowlist is a
second, finer, weaker gate solving the same problem at the price of five app
releases.

## The core refuses ROMs on a number it does not own

`romwbw_disks/tools/build_rom.sh` generates `romwbw_ver.inc` from
`versions/<ver>/version.json` and stamps the HCB at `0x105`/`0x106`
(`build_rom.sh:13,32,53`). `tools/check_source_drift.sh:94` actively guards
against the hardcoded `db 035h` / `db 010h` surviving in the proxy source.

So the release bytes in a published `emu_*.rom` are `romwbw_disks` build data.
This core then reads them back and refuses the ROM if they are not in a list
maintained by hand here. The number is written there and adjudicated here, and
the two places have no way to stay in step except a person editing a macro.

## What this does not fix, stated plainly

v0's third pillar covers the private `0xE1`–`0xEA` block and two standard
functions. It does **not** enumerate the standard RomWBW HBIOS functions
`hbios_dispatch.cc` implements. A future release whose CBIOS calls a standard
function this dispatcher lacks would not be caught by v0 as currently written,
and that is the real risk the release allowlist was standing in for.

Two ways to close it:

1. **Widen v0's pillar 3** to enumerate the whole dispatcher surface, so an
   added requirement is a documented `v1` bump.
2. **Make `boot_test.sh` the gate**, at publish time in `romwbw_disks`, instead
   of a macro at compile time here.

Prefer (2). `romwbw_disks/tools/boot_test.sh` already asserts the CBIOS banner,
the CP/M prompt, no mismatch warning on a matched pair, a warning on a
mismatched one, and an R8/W8 round trip. It is a measurement of the actual pair
rather than a list someone remembered to edit, it runs against the artifact
being published, and it runs at the moment the decision is actually made.

The honest trade: today an entry in `ROMWBW_SUPPORTED_RELEASES` means "somebody
booted *this binary* against that release." Afterwards it means "`romwbw_disks`
booted *a* build of the proxy against that release before publishing it." That
is weaker in principle. It is stronger in practice, because the compile-time
list is edited months before the release it blesses and is never re-checked,
whereas `boot_test.sh` runs every time.

### `boot_test.sh` consumes the gate, so it cannot be left alone

It is already a required publish step — `RELEASING.md:825` is a checklist item
that closes the obvious hole itself ("A SKIP line means no emulator was found,
which is not a pass"), and `RELEASING.md:441-447` already orders
`boot_test.sh 3.7.0` before the `X()` line is added here. Required by the
written procedure, and by no script: neither `publish_release.sh` nor
`verify.yml` runs it.

What it is *not* is independent of the gate. `boot_test.sh:85-98` asks the
binary which releases it can run, by parsing the banner line
`print_version_banner()` prints from `emu_romwbw_supported_list()`. Delete that
and `RUNS` is empty, the pre-v1.39 `RomWBW compatibility: v…` fallback is empty
too, and it `die`s — exit 1 (`common.sh:23`), not a skip. The publish-time gate
would not lose a fact; it would **fail**, blocking every publish, until
`boot_test.sh` is changed to test every published version unconditionally and
to drop its refusal branch (`:210-218`), whose failure message names
`emu_validate_rom_hcb`.

That change is the real step 1. The checklist entry it was waiting for is
already there.

## The one genuine release constant in bank 0

`HBX_BNKCALL` special-cases address `0x0406`, the 3.5.1 device summary. The
comment records what the cruder earlier version cost: RomWBW 3.6.0's
"O - Hardware Monitor" loaded nothing, and every bank call but one was silently
ignored. That is a real release-sensitive constant.

It belongs in `romwbw_ver.inc`, generated by `build_rom.sh` from
`versions/<ver>/version.json`, beside the version bytes that already live
there. Bank 0 is built per release by `romwbw_disks`; the constant should be
too.

**Budget this before cutting it.** `todo.txt`'s `src/emu_hbios.asm` item is
the standing warning: any edit to `src/emu_hbios.asm` moves every published ROM's sha256,
which bumps each version's `generation`, which makes every client invalidate its
cached images — every user re-downloads their disks. `HB_BNKCALL` (`e47c948`,
`d4f4a2a`, 2026-09-06) cost exactly that, a generation-2 republish of both
3.5.1 and 3.6.0. The `0x0406` move should ride along with the next bank-0
change rather than be cut on its own.

## What comes out

Here:

- `ROMWBW_SUPPORTED_RELEASES` (`src/romwbw_pin.h:69-71`)
- the release branch of `emu_validate_rom_hcb` (`src/emu_init.cc:194-211`);
  the HCB marker check and the `CB_PLATFORM` warning stay
- `emu_romwbw_release_supported()` and `emu_romwbw_supported_list()`
  (`src/emu_init.h:108-113`, `src/emu_init.cc:100-133`) — and with the second
  of those, the line `print_version_banner()` prints from it
  (`src/romwbw_emu.cc:806-811`). That line is a cross-repository protocol, not
  a message: `tools/romwbw-get:389-393` names three consumers of it — its own
  `RE_RUNS` (`:395`), `roms/verify_romwbw_pin.sh`, and `romwbw_disks`'
  `tools/boot_test.sh`. Removing the fact removes the line, and removing the
  line is what breaks `boot_test.sh` above.
- `emu_set_allow_untested_romwbw()` / `emu_allow_untested_romwbw()` and the
  `--allow-untested-romwbw` argument (`src/romwbw_emu.cc:1046-1047`) — they
  exist only to escape the gate
- `roms/verify_romwbw_pin.sh`, and its step in `.github/workflows/test.yml`
- in `tools/romwbw-get`: `_runnable()` (`:1417-1436`), the `--allow-untested`
  and `--assume-supported` flags, and the `runnable` default of
  `mirror --versions` — which also stops `mirror` writing `emu_supported` into
  `catalog/manifest.json` (`:1003`, `:1299`), so the web page's RomWBW select
  stops greying anything out (`web/romwbw.html-template:948-952`, covered by
  `tests/web_manifest.js`). `todo.txt`'s `[EMSCRIPTEN]` item about exporting
  `emu_romwbw_supported_list()` to the wasm is the same mechanism seen from the
  other end: it does not survive this.

`ROMWBW_DEFAULT_*` stays. It is a build input for `roms/build_emu_rom.sh` and
the fallback for a ROM whose HCB cannot be read, not a constraint on loading.

In the clients, the per-entry filter and its plumbing:

- `z80cpmw` — `catalogv0::runnableVersions` (`CatalogV0.cpp:452-461`), the
  predicate and empty-set error (`DiskCatalog.cpp:534-562`), the filtered copy
  (`DiskCatalog.cpp:630-632`). `chooseVersion` stays; it stops taking a
  runnable list.
- `ioscpm` — `RomWBWIndex.offered` (`CatalogDocument.swift:370-377`) and the
  bridge method (`RomWBWEmulator.mm:119-124`).
- `cpmdroid` — `runnableRomwbwVersions` (`RomwbwIndex.kt:145-148`) and the JNI
  call (`emu_io_android.cpp:1146`).

In this repository's own documents, which the first draft of this list left out
entirely: `DOWNSTREAM.md`'s "RomWBW Version: runtime, not a pin" section
(`:494-630` — its "New: ROM loads now fail loudly" and "Adding a RomWBW
release" subsections are wholly about the gate), the bullet at `:450-456`, and
the v1.39 migration line at `:1121`; `README.md:154-160` and the
`--allow-untested-romwbw` line in the usage block at `:314`;
`docs/CATALOG.md:31` and `:178`; `roms/build_emu_rom.sh:83`; and the
`verify_romwbw_pin.sh` description in `.github/workflows/test.yml:20-21`.
`docs/ROM_ATTESTATION.md:27` also names the macro, and is the one document here
not to edit in passing: it is a signed filing, and `todo.txt` has a standing
item about what may be changed in it.

In `romwbw_disks` and the clients: `CATALOG_SCHEMA.md:194-203`,
`CLIENT_MIGRATION.md:85-87`, `INTERFACE_V0.md`'s "The one thing v0 could not
fix on its own" section, `RELEASING.md:441-447`, `ioscpm/CLAUDE.md:136-149`,
and in `z80cpmw` `README.md:76-79`, `CLAUDE.md:28-39` and
`FEATURE_PARITY.md:963-978`. Also the 3.6.0 note in the published `index.json`,
which tells clients the `ver_byte` fields exist to be filtered on.

## What stays

`hbios.ver_byte` and `upd_byte` stay in every index entry. They stop being an
emulator gate and remain what they always described: the ROM-to-disk-image
pairing, which is the HBIOS/CBIOS axis and is real. A client may still use them
to refuse to mount a 3.5.1 image under a 3.6.0 ROM.

## Order of work

1. `romwbw_disks` — `boot_test.sh` is already required by `RELEASING.md:825`;
   what is left is to make it survive the deletion: derive the versions to test
   from `versions/` rather than from the emulator's banner, drop the refusal
   branch, and say in `INTERFACE_V0.md` that publishing into the v0 index is the
   assertion that it passed.
2. `romwbw_emu` — delete the gate and the override. One release.
3. The three GUI clients — delete their filters. These can lag; a client that
   still filters is not broken, it just misses out, which is what
   `INTERFACE_V0.md` already says.
4. `0x0406` into `romwbw_ver.inc`, on the next bank-0 change.

Step 2 is what unblocks 3.7.0. Steps 3 and 4 are cleanup.

## Cost, before anyone commits

- Removing the load-time refusal means an unchecked ROM will load and run. The
  protection moves from compile time to publish time — and `boot_test.sh` is
  where it lands, so it has to be taught not to read the thing being deleted
  first. Skip step 1 and the protection does not move to nowhere; the publish
  step exits 1 and nothing can be published at all.
- Every client built before 2026-09-10 compiles in the `catalog-v0` tag by
  name. Those tags stay live forever regardless of anything here.
- GitHub's Latest flag is one flag for the whole repository, and
  `gh release create` claims it by default. The entry point depends on it.
  `CATALOG_SCHEMA.md` §6.2 has the invariant and the way to break it.
