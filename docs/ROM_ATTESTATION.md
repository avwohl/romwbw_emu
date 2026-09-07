# ROM Attestation for Apple App Store Review

## Summary

I, the developer of this application, hereby affirm that I have the appropriate rights and licenses to use the ROM files included with this application and the ROM files it downloads, and I authorize Apple to use these ROMs for testing purposes during App Store review.

## ROM files

Every ROM this application can load is a 512 KB image with the same two-part
construction, the same two copyright holders and the same licence. What varies
between them is only which RomWBW release supplies banks 1-15.

As of v1.40 this repository tracks no ROM image. The ROMs are built and
published by [avwohl/romwbw_disks](https://github.com/avwohl/romwbw_disks), and
`tools/romwbw-get` fetches one and verifies it against the SHA-256 that catalog
publishes for it. See [CATALOG.md](CATALOG.md). The catalog publishes
`emu_avw` - the default ROM, and the one every shipped client uses - for two
RomWBW releases:

- **RomWBW 3.5.1**: `emu_avw-v0-3.5.1.rom`, 524288 bytes, sha256
  `4b11402a29fad22de304775b7c415eb6a74600df06bd57828b9931a7e9693258`.
- **RomWBW 3.6.0**: `emu_avw-v0-3.6.0.rom`, 524288 bytes, sha256
  `01d1ca6d142e9b757d4fd98c2229f2e506dd8c3253839391c8f5d4f6263c6557`.

Both banks 1-15 come from the upstream RomWBW `SBC_simh_std` ROM of the release
named. Both releases are ones this emulator core has been checked against and
will load; the list is `ROMWBW_SUPPORTED_RELEASES` in `src/romwbw_pin.h`, and
the emulator refuses a ROM whose release is not on it.

The same catalog also publishes an alternate ROM id for each release,
`emu_rcz80`, which differs only in taking banks 1-15 from the upstream
`RCZ80_std` ROM instead. Everything below applies to it unchanged.

### Component 1: emu_hbios (Bank 0, 32KB)

**Source:** `src/emu_hbios.asm`
**Copyright:** Original work created by the application developer
**License:** GNU General Public License v3.0
**Rights:** Full copyright ownership - I am the author of this code

This is a minimal HBIOS (Hardware BIOS) proxy that enables the emulator to intercept hardware calls. It contains no third-party code. It is the same code in every ROM listed above; only the RomWBW release it declares differs.

### Component 2: RomWBW System Software (Banks 1-15, 480KB)

**Source:** [RomWBW Project](https://github.com/wwarthen/RomWBW)
**Versions:** 3.5.1 and 3.6.0, one per ROM as listed above
**Copyright:** Wayne Warthen and contributors
**License:** GNU General Public License v3.0
**SPDX Identifier:** GPL-3.0-or-later

RomWBW is open-source system software for Z80/Z180 retro-computing platforms. The GPLv3 license explicitly grants the right to:
- Use the software for any purpose
- Distribute copies of the software
- Modify and distribute modified versions

These banks are taken verbatim from the official RomWBW release packages, which are downloaded and checked against a pinned SHA-256 during the build.

Source code is publicly available at: https://github.com/wwarthen/RomWBW

## License Compliance

This application complies with GPLv3 requirements:

- Source for the emulator, for Component 1 (`src/emu_hbios.asm`), and the build
  script that assembles it into bank 0 (`roms/build_emu_rom.sh`) is published at:
  https://github.com/avwohl/romwbw_emu
- The published ROM images are built and released by
  https://github.com/avwohl/romwbw_disks, which holds the scripts that cut every
  one of them and the manifests pinning each upstream RomWBW package by SHA-256.
- The LICENSE file (GPLv3) is included in both repositories.
- Each published ROM's SHA-256 is recorded in the public catalog alongside it, so
  a downloaded ROM can be checked against the source it was built from.

Build scripts to reproduce the ROM from source are provided, and that claim is
checkable rather than merely stated. `roms/build_emu_rom.sh` assembles
`src/emu_hbios.asm` into bank 0, overlays it on banks 1-15 taken from the
upstream RomWBW package, hashes the result, and compares that against the
SHA-256 the catalog publishes for the same ROM. Run on 2026-09-07 it ends:

```
PASS: byte-identical to the published emu_avw for RomWBW 3.5.1
      sha256 4b11402a29fad22de304775b7c415eb6a74600df06bd57828b9931a7e9693258
      The ROM users download is reproducible from the source in this
      repository.  That is what docs/ROM_ATTESTATION.md asserts.
```

A reviewer with `um80` and a checkout can therefore reproduce the exact bytes of
a ROM the application downloads, and be told by the script whether they match,
instead of comparing hashes by hand.

That script reproduces RomWBW 3.5.1 only: this repository's `src/emu_hbios.asm`
hardcodes its version stamp, so bank 0 as built here can be overlaid on 3.5.1's
banks 1-15 and on no others, and the script refuses any other source ROM rather
than building a mismatched image. The parameterised copy of the same source,
which builds bank 0 for any release, is in romwbw_disks alongside the script
that cuts what users download; romwbw_disks' `tools/check_source_drift.sh`
asserts the two trees' copies have not diverged.

## Authorization for Apple

I hereby grant Apple Inc. permission to use the ROM file included with this application (`emu_avw.rom`), and any ROM the application downloads from https://github.com/avwohl/romwbw_disks/releases/, for the purpose of testing and reviewing this application for the App Store.

## Contact

Developer: Aaron Wohl
Repositories:
  https://github.com/avwohl/romwbw_emu (emulator, and the source of bank 0)
  https://github.com/avwohl/romwbw_disks (the published ROM images and their build scripts)
Date: 2026-09-07

---

**Digital Signature:** This attestation is submitted as part of App Store review for iOSCPM (CP/M Emulator).
