/*
 * romwbw_pin.h - which RomWBW releases this emulator core can run
 *
 * There are two different things in this file, and until now they were one
 * number doing both jobs:
 *
 *   1. SUPPORTED - the releases this core has been checked against and will
 *      load.  This is a LIST.  It answers "can this binary boot that ROM",
 *      and it is the only thing emu_validate_rom_hcb() consults.
 *
 *   2. DEFAULT - the release the artifacts bundled in THIS TREE (roms/,
 *      disks/) are cut from.  Build scripts use it to pick the upstream ROM
 *      to overlay bank 0 onto, and to name what they built.  It says nothing
 *      about what the binary can load.
 *
 * What is deliberately NOT here any more is a compile-time answer to "what
 * RomWBW version is running".  That is read out of the loaded ROM's HBIOS
 * Configuration Block every time it is asked - see emu_romwbw_release_loaded()
 * in emu_init.h.  Deriving it, rather than storing it, is what makes the five
 * places that report a version to the guest incapable of disagreeing with the
 * ROM or with each other: there is no cached copy to go stale and no
 * initialisation order to get wrong.
 *
 * Why a supported LIST and not "load anything":
 *
 * Banks 1-15 of an emu_*.rom are upstream RomWBW, but bank 0 is ours - a
 * proxy that turns HBIOS calls into port I/O, backed by a C++ dispatcher that
 * implements a specific set of HBIOS functions.  A future release whose CBIOS
 * or ROM applications call something the dispatcher does not implement would
 * load and then misbehave.  Adding a release here is therefore a claim that
 * somebody ran it, and the entries below say who and when.
 *
 * Adding a release:
 *
 *   1. Build its emu_*.rom and disk images (that is romwbw_disks' job:
 *      tools/build_all.sh <ver>).
 *   2. Boot them.  romwbw_disks/tools/boot_test.sh asserts the CBIOS banner,
 *      the CP/M prompt, no version-mismatch warning on a matched pair, and a
 *      warning on a mismatched one.
 *   3. Round-trip a file through R8/W8 - that exercises the private
 *      0xE1-0xEA host block, which upstream knows nothing about.
 *   4. Add the X() line below with the date it was checked.
 *
 * Removing the compile-time pin does NOT remove the pairing rule: a ROM and
 * the boot slice of a disk image still have to come from the same release, or
 * the guest's CBIOS prints
 *
 *     *** WARNING: HBIOS/CBIOS Version Mismatch ***
 *
 * That warning is now the only thing enforcing the pairing, and it comes from
 * the guest, not from us - which is why emu_validate_rom_hcb() still refuses
 * a ROM it has never been checked against rather than letting the guest sort
 * it out.
 */

#ifndef ROMWBW_PIN_H
#define ROMWBW_PIN_H

/*
 * The releases this core has been checked against, as
 * X(major, minor, update, patch, "checked" note).
 *
 * RomWBW packs a version into two bytes: ver = major<<4 | minor,
 * upd = update<<4 | patch.  v3.5.1 is 35 10, v3.6.0 is 36 00.  Those two
 * bytes live at HCB offsets 0x05/0x06 (absolute 0x105/0x106 in ROM bank 0),
 * in the HBIOS ident block, in the CBIOS page-zero stamp at 0x42/0x43, in
 * what HBF_SYSVER returns in DE, and in the NVRAM checksum seed.
 */
#define ROMWBW_SUPPORTED_RELEASES(X)                                          \
  X(3, 5, 1, 0, "2025-05-21 release; checked 2026-09-05")                     \
  X(3, 6, 0, 0, "2026-03-28 release; checked 2026-09-05")

/*
 * The release the ROM and disk artifacts bundled in this tree are cut from.
 * roms/build_emu_rom.sh, roms/build_from_source.sh and
 * roms/verify_romwbw_pin.sh read these four numbers out of this file with
 * sed, so keep the "#define NAME value" shape.
 *
 * This must be one of the supported releases above; emu_validate_rom_hcb()
 * would refuse the tree's own ROM otherwise.
 */
#define ROMWBW_DEFAULT_MAJOR 3
#define ROMWBW_DEFAULT_MINOR 5
#define ROMWBW_DEFAULT_UPDATE 1
#define ROMWBW_DEFAULT_PATCH 0
#define ROMWBW_DEFAULT_STR "3.5.1"

#define ROMWBW_DEFAULT_VER_BYTE ((ROMWBW_DEFAULT_MAJOR << 4) | ROMWBW_DEFAULT_MINOR)
#define ROMWBW_DEFAULT_UPD_BYTE ((ROMWBW_DEFAULT_UPDATE << 4) | ROMWBW_DEFAULT_PATCH)

#endif  // ROMWBW_PIN_H
