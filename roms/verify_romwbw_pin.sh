#!/bin/sh
#
# Verify that every RomWBW artifact in a tree names a release the emulator
# core can actually run, and that the ROMs and disks in it are pairable - so a
# downstream client can confirm in one command that what it is about to ship
# will boot.
#
# THE SINGLE PIN IS GONE. The core no longer compiles in one RomWBW version:
# the version a guest sees is read out of the loaded ROM's HCB at run time,
# and one binary boots any release in ROMWBW_SUPPORTED_RELEASES
# (src/romwbw_pin.h). So the question this script answers changed from
#
#   "does everything here match the one pinned release?"          (before)
#   "is everything here a release this core supports, and do the   (now)
#    disks have a ROM to pair with?"
#
# A tree shipping BOTH 3.5.1 and 3.6.0 artifacts is now correct, and used to
# be a failure. What is still a failure is an artifact from a release nobody
# has checked this core against, because that is the one that loads and then
# misbehaves.
#
# What is read:
#
#   roms/*.rom, *.bin  HCB at 0x103: marker 'W' 0xA8, then the version bytes,
#                      then CB_PLATFORM (0 = EMU, anything else is a stock
#                      ROM for real hardware and will not run the proxy)
#   disks/*.img        boot slices carry a CBIOS that prints
#                      "CBIOS v<ver> [WBW]".  A disk whose release has no ROM
#                      beside it is the mixed pair that makes a guest print
#                      "*** WARNING: HBIOS/CBIOS Version Mismatch ***"
#   src/romwbw_emu     the built binary lists the releases it can run in
#                      --version; that list has to match this header, or the
#                      binary is stale
#
# Usage: roms/verify_romwbw_pin.sh [tree_root]
# Exit:  0 all checks passed, 1 at least one mismatch
#
# tree_root defaults to this repo. Point it at a downstream port to check
# what that port is about to ship:
#
#   romwbw_emu/roms/verify_romwbw_pin.sh ../z80cpmw
#
# The supported list always comes from this script's own checkout, because it
# is a property of the core, not of the tree being checked - a client tree has
# no src/romwbw_pin.h of its own. Override with ROMWBW_PIN_H=/path/to/header.
#
# The name is kept for the callers that already run it (DOWNSTREAM.md, three
# client CHANGELOGs, README.md); there is no pin left for it to verify.

set -u

SELF_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="${1:-$SELF_ROOT}"
# Resolved to an absolute path so the -path prunes below match whatever form
# the caller typed ("../z80cpmw", "z80cpmw/", "."), and so relname() strips a
# prefix that is actually there.
if [ -d "$ROOT" ]; then
    ROOT="$(cd "$ROOT" && pwd)"
fi
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

# Pull the tree's default release out of the header rather than duplicating
# it here.  That is the release roms/ and disks/ in THIS tree are cut from -
# it is not a constraint on what the binary can load.
pin_field() {
    sed -n "s/^#define $1 \([0-9]*\).*/\1/p" "$PIN_H" | head -1
}
MAJOR=$(pin_field ROMWBW_DEFAULT_MAJOR)
MINOR=$(pin_field ROMWBW_DEFAULT_MINOR)
UPDATE=$(pin_field ROMWBW_DEFAULT_UPDATE)
PATCH=$(pin_field ROMWBW_DEFAULT_PATCH)
DEFAULT_STR=$(sed -n 's/^#define ROMWBW_DEFAULT_STR "\(.*\)".*/\1/p' "$PIN_H" | head -1)

if [ -z "$MAJOR" ] || [ -z "$MINOR" ] || [ -z "$UPDATE" ] || [ -z "$PATCH" ] ||
   [ -z "$DEFAULT_STR" ]; then
    echo "Error: could not parse ROMWBW_DEFAULT_* out of $PIN_H" >&2
    exit 1
fi

# The header's own string has to agree with its own numbers, or every message
# printed from it lies about which release it means.
DERIVED="$MAJOR.$MINOR.$UPDATE"
if [ "$PATCH" -ne 0 ]; then
    DERIVED="$DERIVED.$PATCH"
fi
if [ "$DERIVED" != "$DEFAULT_STR" ]; then
    bad "src/romwbw_pin.h" "ROMWBW_DEFAULT_STR is \"$DEFAULT_STR\" but the numbers say $DERIVED"
fi

# The supported releases, from the one X-macro list the C++ also expands.  Two
# parallel forms, because the ROM check has HCB bytes in hand and the disk
# check has a dotted version string:
#
#   SUPPORTED_HEX   "3510 3600"    ver||upd, as they appear in the HCB
#   SUPPORTED_STR   "3.5.1 3.6.0"  as CBIOS prints them in a boot slice
#
# A header whose list cannot be parsed is a hard error rather than an empty
# list: an empty list would fail every artifact, which reads like a broken
# tree instead of a broken script.
SUPPORTED_HEX=$(sed -n 's/^  *X(\([0-9]*\), *\([0-9]*\), *\([0-9]*\), *\([0-9]*\).*/\1 \2 \3 \4/p' "$PIN_H" |
                while read -r a b c d; do printf '%x%x%x%x ' "$a" "$b" "$c" "$d"; done)
SUPPORTED_STR=$(sed -n 's/^  *X(\([0-9]*\), *\([0-9]*\), *\([0-9]*\), *\([0-9]*\).*/\1 \2 \3 \4/p' "$PIN_H" |
                while read -r a b c d; do
                    if [ "$d" -eq 0 ]; then printf '%d.%d.%d ' "$a" "$b" "$c"
                    else printf '%d.%d.%d.%d ' "$a" "$b" "$c" "$d"; fi
                done)
if [ -z "$SUPPORTED_HEX" ] || [ -z "$SUPPORTED_STR" ]; then
    echo "Error: could not parse ROMWBW_SUPPORTED_RELEASES out of $PIN_H" >&2
    exit 1
fi

# Is "$1" (four hex nibbles, or a dotted version) in the corresponding list?
supported_hex() {
    for _s in $SUPPORTED_HEX; do [ "$_s" = "$1" ] && return 0; done
    return 1
}
supported_str() {
    for _s in $SUPPORTED_STR; do [ "$_s" = "$1" ] && return 0; done
    return 1
}

# The tree's own default has to be one this core can run, or the tree cannot
# boot what it ships.
DEFAULT_HEX=$(printf '%x%x%x%x' "$MAJOR" "$MINOR" "$UPDATE" "$PATCH")
if ! supported_hex "$DEFAULT_HEX"; then
    bad "src/romwbw_pin.h" "ROMWBW_DEFAULT_STR v$DEFAULT_STR is not in ROMWBW_SUPPORTED_RELEASES"
fi

# Which releases the tree actually contains, filled in as ROMs and disks are
# read, so the pairing check at the end has something to compare.
ROM_RELEASES=""
DISK_RELEASES=""

echo "RomWBW releases this core can run:$(printf ' v%s' $SUPPORTED_STR)"
echo "This tree's artifacts default to:  v$DEFAULT_STR"
echo

# --- ROM images -------------------------------------------------------------
# Search the whole tree rather than a fixed roms/ directory: ports put these
# where their packaging wants them (cpmdroid ships its ROM in
# app/src/main/assets/), and a check that silently finds nothing is worse
# than no check. Results go through a temp file so the loop below is not a
# subshell and the fail/warn counters survive it.
# archive/ is skipped along with .git: it exists to hold superseded material
# on purpose - archive/cpm22/ still carries an assembled cpm22.bin, which this
# script's *.bin search would otherwise pick up and fail on forever, because a
# 1970s CP/M binary has no HBIOS configuration block to name a release.
#
# So is any sibling checkout sitting in the tree root, and that one is not
# cosmetic. Every job in .github/workflows/test.yml and release.yml does
# `git clone https://github.com/avwohl/cpmemu.git` in the workspace root -
# which is the tree root this script is pointed at - and cpmemu/tests holds
# seventeen hand-written Z80 .bin fixtures of a few hundred bytes each. Without
# this prune all seventeen are reported as "too small to contain an HCB" and
# the run exits 1: seventeen mismatches against the pin in a tree where nothing
# is wrong. release.yml clones emsdk beside it for the same reason, and a port
# following the same recipe can land any of the rest of this family there.
#
# Two conditions, and both are needed. The prune matches a full PATH rather
# than a name, because a name prune reaches any depth: z80cpmw keeps its own
# sources in a z80cpmw/ subdirectory, which is exactly the tree this script is
# meant to read when it is pointed at that port
# (`romwbw_emu/roms/verify_romwbw_pin.sh ../z80cpmw`). And the directory has to
# be a checkout in its own right - it has a .git of its own - because that is
# what makes it somebody else's tree rather than part of this one. A tree root
# that happens to be named cpmemu is then still checked in full.
scratch=$(mktemp -d 2>/dev/null || mktemp -d -t rwbwpin)
trap 'rm -rf "$scratch"' EXIT INT TERM
# Built in the positional parameters rather than one string: a tree root with
# a space in it has to survive into find's expression intact, and "$@" is the
# only POSIX list that does. The script's own argument was read into ROOT
# above, so there is nothing left in "$@" to lose.
set -- -name .git -o -name archive
for sibling in cpmemu emsdk romwbw_emu z80cpmw cpmdroid ioscpm RomWBW; do
    # -e, not -d: a worktree or submodule checkout has .git as a file.
    if [ -e "$ROOT/$sibling/.git" ]; then
        set -- "$@" -o -path "$ROOT/$sibling"
    fi
done

find "$ROOT" \( "$@" \) -prune -o \
    -type f \( -name '*.rom' -o -name '*.bin' \) \
    -print 2>/dev/null | sort > "$scratch/roms"
find "$ROOT" \( "$@" \) -prune -o \
    -type f -name '*.img' \
    -print 2>/dev/null | sort > "$scratch/disks"

# Label a file by its path relative to the tree root, so two ROMs with the
# same basename in different directories stay distinguishable.
relname() {
    printf '%s' "${1#"$ROOT"/}"
}

echo "ROM images (*.rom, *.bin):"
found_rom=0
while IFS= read -r f; do
    [ -f "$f" ] || continue
    found_rom=1
    name=$(relname "$f")

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
    got_major=$(printf '%d' "0x$(echo "$ver" | cut -c1)")
    got_minor=$(printf '%d' "0x$(echo "$ver" | cut -c2)")
    got_upd=$(printf '%d' "0x$(echo "$upd" | cut -c1)")
    got_pat=$(printf '%d' "0x$(echo "$upd" | cut -c2)")
    if [ "$got_pat" -eq 0 ]; then
        got_str="$got_major.$got_minor.$got_upd"
    else
        got_str="$got_major.$got_minor.$got_upd.$got_pat"
    fi
    if ! supported_hex "$ver$upd"; then
        bad "$name" "built for RomWBW v$got_str, which this core has not been checked against (it can run$(printf ' v%s' $SUPPORTED_STR))"
        continue
    fi
    # A stock ROM is not runnable here, but it is the build input that
    # build_emu_rom.sh overlays our bank 0 onto. Warn so nobody points
    # --romwbw at one by mistake.
    if [ "$plat" != "00" ]; then
        warned "$name" "CB_PLATFORM=0x$plat - stock hardware ROM for v$got_str (build input only, not runnable)"
        continue
    fi
    case " $ROM_RELEASES " in *" $got_str "*) ;; *) ROM_RELEASES="$ROM_RELEASES $got_str" ;; esac
    ok "$name" "emulator ROM for RomWBW v$got_str"
done < "$scratch/roms"
[ "$found_rom" -eq 1 ] || note "(none found)"
echo

# --- Disk images ------------------------------------------------------------
# grep -a so a binary image is searched as text on both GNU and BSD grep.
echo "Disk images (*.img):"
found_disk=0
while IFS= read -r f; do
    [ -f "$f" ] || continue
    found_disk=1
    name=$(relname "$f")

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
    # Every slice in one image has to come from ONE release: a combo whose
    # slices disagree cannot be paired with any single ROM, so it is broken no
    # matter which one it is booted against.
    unsupported=""
    count=0
    for v in $versions; do
        count=$((count + 1))
        supported_str "$v" || unsupported="$unsupported $v"
        case " $DISK_RELEASES " in *" $v "*) ;; *) DISK_RELEASES="$DISK_RELEASES $v" ;; esac
    done
    if [ -n "$unsupported" ]; then
        bad "$name" "boot slice CBIOS v$(echo "$unsupported" | sed 's/^ //'), which this core has not been checked against"
    elif [ "$count" -gt 1 ]; then
        bad "$name" "slices carry more than one CBIOS version ($(echo "$versions" | tr '\n' ' ' | sed 's/ $//')) - no single ROM pairs with this image"
    else
        ok "$name" "boot slice CBIOS v$versions"
    fi
done < "$scratch/disks"
[ "$found_disk" -eq 1 ] || note "(none found)"
echo

# --- Pairing ----------------------------------------------------------------
# A ROM and the boot slice of the disk beside it must be the same release, or
# the guest prints "*** WARNING: HBIOS/CBIOS Version Mismatch ***" and may
# misbehave.  Nothing in the core enforces that any more - it cannot, since
# the whole point is that one binary runs either - so it is enforced here.
#
# A disk whose release has no ROM in the tree is the shipping hazard: the user
# gets an image that nothing bundled can boot cleanly.  The reverse (a ROM
# with no disks) is normal - ports bundle a ROM and download disks.
echo "Pairing:"
if [ -z "$DISK_RELEASES" ] && [ -z "$ROM_RELEASES" ]; then
    note "(no RomWBW artifacts found in this tree)"
else
    for v in $DISK_RELEASES; do
        case " $ROM_RELEASES " in
        *" $v "*) ok "RomWBW v$v" "disks and a ROM, matched" ;;
        *)        warned "RomWBW v$v" "disk images but no v$v ROM in this tree - whatever ROM is used, the guest will warn about an HBIOS/CBIOS mismatch unless one is downloaded" ;;
        esac
    done
    for v in $ROM_RELEASES; do
        case " $DISK_RELEASES " in
        *" $v "*) ;;
        *)        note "info  RomWBW v$v: a ROM but no disk images - normal for a port that downloads them" ;;
        esac
    done
fi
echo

# --- Built binary -----------------------------------------------------------
echo "Built emulator:"
EMU="$ROOT/src/romwbw_emu"
if [ -x "$EMU" ]; then
    # The binary prints its supported list; it must be the same list this
    # header declares, or the binary was built before the header changed.
    # This is the check that catches a stale build - and it used to be able
    # to miss one entirely, because src/makefile had no header dependencies
    # at all until -MMD was added, so editing this header rebuilt nothing.
    reported=$("$EMU" --version 2>&1 |
               sed -n 's/^RomWBW releases this build can run: \(.*\)/\1/p' | head -1)
    expected=$(echo "$SUPPORTED_STR" | sed 's/ $//; s/ /, /g')
    if [ -z "$reported" ]; then
        bad "src/romwbw_emu" "--version does not list any RomWBW releases (built from an older source tree?)"
    elif [ "$reported" != "$expected" ]; then
        bad "src/romwbw_emu" "binary runs [$reported], header says [$expected] - rebuild (make -C src)"
    else
        ok "src/romwbw_emu" "runs $reported"
    fi
else
    note "info  not built (run 'make -C src') - skipping"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS: every artifact names a RomWBW release this core can run ($warn warning(s))"
    exit 0
fi
echo "FAIL: $fail problem(s) ($warn warning(s))"
exit 1
