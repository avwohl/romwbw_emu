/*
 * romwbw_pin.h - the RomWBW release this emulator core is pinned to
 *
 * SINGLE SOURCE OF TRUTH. Everything that has to agree on a RomWBW version
 * derives from the four numbers below:
 *
 *   - the HBIOS version this core reports to the guest (HBF_SYSVER)
 *   - the NVRAM checksum seed, which RomWBW's SYSCONF mixes the version into
 *   - the HCB and ident-block version bytes assembled into src/emu_hbios.asm
 *     (assembly cannot include this header - roms/verify_romwbw_pin.sh checks
 *     the assembled bytes in the built ROM against these values)
 *   - the boot slices inside the disk images in disks/, whose CBIOS prints
 *     "CBIOS v<pin> [WBW]" and warns on a mismatch with the HBIOS above
 *   - the ROM that roms/build_from_source.sh overlays our bank 0 onto
 *
 * Why the pin exists: our C++ HBIOS emulates one specific RomWBW release.
 * Boot a slice built by a different release and its CBIOS prints
 * "*** WARNING: HBIOS/CBIOS Version Mismatch ***" and may misbehave, so a
 * downstream client has to build against a matching ROM and disk set. Run
 * roms/verify_romwbw_pin.sh to confirm a tree does.
 *
 * Changing the pin is not a version bump: it means re-cutting emu_*.rom from
 * the new RomWBW release, refreshing every image in disks/, re-checking the
 * HBIOS functions this core implements against that release's proto.asm, and
 * telling downstream ports to ship the new disk images. See the "RomWBW
 * version pin" section of ../DOWNSTREAM.md.
 */

#ifndef ROMWBW_PIN_H
#define ROMWBW_PIN_H

// RomWBW release this core emulates: https://github.com/wwarthen/RomWBW
// v3.5.1, released 2025-05-21.
#define ROMWBW_PIN_MAJOR 3
#define ROMWBW_PIN_MINOR 5
#define ROMWBW_PIN_UPDATE 1
#define ROMWBW_PIN_PATCH 0
#define ROMWBW_PIN_STR "3.5.1"

// RomWBW packs the version into two BCD-ish bytes: high nibble major /
// update, low nibble minor / patch. Used by the HCB (offset 0x05-0x06), the
// HBIOS ident block, HBF_SYSVER (D=first byte, E=second) and the NVRAM
// checksum seed.
#define ROMWBW_PIN_VER_BYTE ((ROMWBW_PIN_MAJOR << 4) | ROMWBW_PIN_MINOR)
#define ROMWBW_PIN_UPD_BYTE ((ROMWBW_PIN_UPDATE << 4) | ROMWBW_PIN_PATCH)
#define ROMWBW_PIN_DE ((ROMWBW_PIN_VER_BYTE << 8) | ROMWBW_PIN_UPD_BYTE)

#endif  // ROMWBW_PIN_H
