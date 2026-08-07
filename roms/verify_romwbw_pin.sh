#!/bin/sh
#
# Verify that every artifact in this tree agrees with the pinned RomWBW
# release, so a downstream client can confirm in one command that the ROM and
# disk images it is about to ship match the core it is building against.
#
# The pin lives in src/romwbw_pin.h and is the single source of truth. This
# script re-derives everything from it:
#
#   roms/*.rom, *.bin  HCB at 0x103: marker 'W' 0xA8, then the version bytes,
#                      then CB_PLATFORM (0 = EMU, anything else is a stock
#                      ROM for real hardware and will not run the proxy)
#   disks/*.img        boot slices carry a CBIOS that prints
#                      "CBIOS v<pin> [WBW]"; a different version there means
#                      the guest will warn about an HBIOS/CBIOS mismatch
#   src/romwbw_emu     the built binary reports its pin in --version
#
# Usage: roms/verify_romwbw_pin.sh [tree_root]
# Exit:  0 all checks passed, 1 at least one mismatch
#
# tree_root defaults to this repo. Point it at a downstream port to check
# what that port is about to ship:
#
#   romwbw_emu/roms/verify_romwbw_pin.sh ../z80cpmw
#
# The pin always comes from this script's own checkout, because the pin is a
# property of the core, not of the tree being checked - a client tree has no
# src/romwbw_pin.h of its own. Override with ROMWBW_PIN_H=/path/to/header.

set -u

SELF_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="${1:-$SELF_ROOT}"
PIN_H="${ROMWBW_PIN_H:-$SELF_ROOT/src/romwbw_pin.h}"

fail=0
warn=0

note()  { printf '  %s\n' "$*"; }
ok()    { printf 'ok    %-28s %s\n' "$1" "$2"; }
bad()   { printf 'FAIL  %-28s %s\n' "$1" "$2"; fail=$((fail + 1)); }
warned(){ printf 'warn  %-28s %s\n' "$1" "$2"; warn=$((warn + 1)); }

if [ ! -f "$PIN_H" ]; then
    echo "Error: cannot find the pin at $PIN_H" >&2
    exit 1
fi

# Pull the four numbers out of the header rather than duplicating them here.
pin_field() {
    sed -n "s/^#define $1 \([0-9]*\).*/\1/p" "$PIN_H" | head -1
}
MAJOR=$(pin_field ROMWBW_PIN_MAJOR)
MINOR=$(pin_field ROMWBW_PIN_MINOR)
UPDATE=$(pin_field ROMWBW_PIN_UPDATE)
PATCH=$(pin_field ROMWBW_PIN_PATCH)
PIN_STR=$(sed -n 's/^#define ROMWBW_PIN_STR "\(.*\)".*/\1/p' "$PIN_H" | head -1)

if [ -z "$MAJOR" ] || [ -z "$MINOR" ] || [ -z "$UPDATE" ] || [ -z "$PATCH" ] ||
   [ -z "$PIN_STR" ]; then
    echo "Error: could not parse the pin out of $PIN_H" >&2
    exit 1
fi

# The header's own string has to agree with its own numbers, or every message
# printed from the pin lies about which release it means.
DERIVED="$MAJOR.$MINOR.$UPDATE"
if [ "$PATCH" -ne 0 ]; then
    DERIVED="$DERIVED.$PATCH"
fi
if [ "$DERIVED" != "$PIN_STR" ]; then
    bad "src/romwbw_pin.h" "ROMWBW_PIN_STR is \"$PIN_STR\" but the numbers say $DERIVED"
fi

EXP_VER=$(printf '%x%x' "$MAJOR" "$MINOR")
EXP_UPD=$(printf '%x%x' "$UPDATE" "$PATCH")

echo "RomWBW pin: v$PIN_STR  (HCB version bytes $EXP_VER $EXP_UPD)"
echo

# --- ROM images -------------------------------------------------------------
echo "ROM images (roms/*.rom, *.bin):"
found_rom=0
for f in "$ROOT"/roms/*.rom "$ROOT"/roms/*.bin; do
    [ -f "$f" ] || continue
    found_rom=1
    name=$(basename "$f")

    # HCB is at 0x100; we need 0x103..0x107 (marker, marker, ver, upd, plat).
    bytes=$(od -An -tx1 -j 259 -N 5 "$f" 2>/dev/null | tr -s ' ' | sed 's/^ //')
    if [ -z "$bytes" ]; then
        bad "$name" "too small to contain an HCB"
        continue
    fi
    m0=$(echo "$bytes" | cut -d' ' -f1)
    m1=$(echo "$bytes" | cut -d' ' -f2)
    ver=$(echo "$bytes" | cut -d' ' -f3)
    upd=$(echo "$bytes" | cut -d' ' -f4)
    plat=$(echo "$bytes" | cut -d' ' -f5)

    # A shippable .rom must carry an HCB. A .bin may be a build intermediate
    # that legitimately has none (romldr.bin is bank 1, emu_rom.bin is a
    # stub), so only flag those when the marker is half-right: a leading 'W'
    # with a wrong second byte is a damaged HCB, not an absent one - that is
    # exactly how the stale emu_hbios.bin (0xB8) and the emu_rcz80.rom built
    # from it were found.
    if [ "$m0" != "57" ] || [ "$m1" != "a8" ]; then
        case "$name" in
        *.rom)
            bad "$name" "bad HCB marker $m0 $m1 (expected 57 a8) - corrupt or not a RomWBW ROM"
            ;;
        *)
            if [ "$m0" = "57" ]; then
                bad "$name" "damaged HCB marker 57 $m1 (expected 57 a8) - stale or corrupt build artifact"
            else
                note "info  $name: no HCB - build intermediate, not a bank 0 image"
            fi
            ;;
        esac
        continue
    fi
    if [ "$ver" != "$EXP_VER" ] || [ "$upd" != "$EXP_UPD" ]; then
        got_major=$(printf '%d' "0x$(echo "$ver" | cut -c1)")
        got_minor=$(printf '%d' "0x$(echo "$ver" | cut -c2)")
        got_upd=$(printf '%d' "0x$(echo "$upd" | cut -c1)")
        bad "$name" "built for RomWBW v$got_major.$got_minor.$got_upd, pin is v$PIN_STR"
        continue
    fi
    # A stock ROM is not runnable here, but it is the build input that
    # build_emu_rom.sh overlays our bank 0 onto, so its banks 1-15 still have
    # to come from the pinned release - which the version check above already
    # enforced. Warn so nobody points --romwbw at one by mistake.
    if [ "$plat" != "00" ]; then
        warned "$name" "CB_PLATFORM=0x$plat - stock hardware ROM (build input only, not runnable)"
        continue
    fi
    ok "$name" "emulator ROM for RomWBW v$PIN_STR"
done
[ "$found_rom" -eq 1 ] || note "(none found)"
echo

# --- Disk images ------------------------------------------------------------
# grep -a so a binary image is searched as text on both GNU and BSD grep.
echo "Disk images (disks/*.img):"
found_disk=0
for f in "$ROOT"/disks/*.img; do
    [ -f "$f" ] || continue
    found_disk=1
    name=$(basename "$f")

    # Anything below one hd1k slice is not a disk image at all - ports keep
    # OS images (cpm_wbw.img, zsys_wbw.img) next to real disks, and calling
    # those "not bootable" reads like a fault when it is just a file type.
    fsize=$(wc -c < "$f" | tr -d ' ')
    if [ "$fsize" -lt 8388608 ]; then
        note "info  $name: $fsize bytes - too small to be a disk image, not checked"
        continue
    fi

    versions=$(grep -a -o 'CBIOS v[0-9][0-9.]* \[WBW\]' "$f" 2>/dev/null |
               sed 's/CBIOS v//; s/ \[WBW\]//' | sort -u)
    if [ -z "$versions" ]; then
        note "info  $name: no CBIOS in any slice - data-only disk, not bootable"
        continue
    fi
    mismatch=""
    for v in $versions; do
        [ "$v" = "$PIN_STR" ] || mismatch="$mismatch $v"
    done
    if [ -n "$mismatch" ]; then
        bad "$name" "boot slice CBIOS version(s)$mismatch, pin is v$PIN_STR"
    else
        ok "$name" "boot slice CBIOS v$PIN_STR"
    fi
done
[ "$found_disk" -eq 1 ] || note "(none found)"
echo

# --- Built binary -----------------------------------------------------------
echo "Built emulator:"
EMU="$ROOT/src/romwbw_emu"
if [ -x "$EMU" ]; then
    reported=$("$EMU" --version 2>&1 |
               sed -n 's/^RomWBW compatibility: v\([0-9.]*\).*/\1/p' | head -1)
    if [ -z "$reported" ]; then
        bad "src/romwbw_emu" "--version does not report a RomWBW pin (stale build?)"
    elif [ "$reported" != "$PIN_STR" ]; then
        bad "src/romwbw_emu" "binary reports v$reported, pin is v$PIN_STR - rebuild"
    else
        ok "src/romwbw_emu" "reports RomWBW v$PIN_STR"
    fi
else
    note "info  not built (run 'make -C src') - skipping"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS: everything checked agrees with RomWBW v$PIN_STR ($warn warning(s))"
    exit 0
fi
echo "FAIL: $fail mismatch(es) against RomWBW v$PIN_STR ($warn warning(s))"
exit 1
