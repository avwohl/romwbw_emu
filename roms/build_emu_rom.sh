#!/bin/sh
#
# build_emu_rom.sh - assemble src/emu_hbios.asm and overlay it on a RomWBW ROM,
# then prove the result is bit-for-bit what romwbw_disks publishes.
#
# This repository does not ship a ROM any more; it ships the source of bank 0
# and this script, which is what makes the published ROM reproducible from that
# source.  docs/ROM_ATTESTATION.md and the GPL both turn on that being true, so
# the script's real output is not the file - it is the hash comparison at the
# end.  romwbw_disks/tools/build_rom.sh is what actually cuts what users get.
#
#   bank 0      32 KB, assembled here from src/emu_hbios.asm - the HBIOS proxy
#   banks 1-15 480 KB, lifted verbatim from an upstream RomWBW ROM
#
# Usage: roms/build_emu_rom.sh [-o OUTPUT] [--rom-id ID] [SOURCE_ROM]
#
#   SOURCE_ROM   a 512 KB stock RomWBW ROM supplying banks 1-15.  With none
#                given, the upstream Package.zip named by the catalog is
#                fetched (and sha256-checked) with tools/romwbw-get, and the
#                file the catalog's built_from.banks_1_15 names is taken out
#                of it.
#   -o OUTPUT    where to write.  Default: a temporary directory, printed at
#                the end.  This script will NOT write inside the repository -
#                a built ROM in the tree is exactly what was removed.
#   --rom-id ID  which catalog ROM to reproduce (default: the one flagged
#                default:true, today emu_avw).
#
# THE 3.5.1 LIMIT, which is the whole reason this is not a general builder:
# src/emu_hbios.asm hardcodes its version stamp - `db 035h` / `db 010h` at
# CB_VERSION and again in the ident block.  romwbw_disks' copy of the same file
# reads those from a generated romwbw_ver.inc, which is how it builds bank 0
# for any release, and its tools/check_source_drift.sh asserts that this copy
# stays hardcoded.  So this script can only reproduce the release named by
# ROMWBW_DEFAULT_* in src/romwbw_pin.h.  Overlaying this bank 0 on another
# release's banks 1-15 produces a ROM that loads and then prints
# "*** WARNING: HBIOS/CBIOS Version Mismatch ***", so it is refused rather
# than built.
#
# Requires um80 + ul80 (pip install um80) to build, and python3 to CHECK - the
# catalog is read through tools/romwbw-get, which is a python3 script, and the
# entry is parsed by python3 here.  Without it the ROM is still built and the
# hash comparison - the only thing that makes running this worthwhile - is
# skipped, and the script says so.  unzip is needed only when it has to fetch
# the upstream package for you.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
SRC_DIR="$ROOT/src"
PIN_H="$SRC_DIR/romwbw_pin.h"
GET="$ROOT/tools/romwbw-get"

UM80="${UM80:-um80}"
UL80="${UL80:-ul80}"

OUTPUT=""
ROM_ID=""
SOURCE_ROM=""

while [ $# -gt 0 ]; do
    case "$1" in
        -o) OUTPUT="$2"; shift 2 ;;
        -o*) OUTPUT="${1#-o}"; shift ;;
        --rom-id) ROM_ID="$2"; shift 2 ;;
        --rom-id=*) ROM_ID="${1#--rom-id=}"; shift ;;
        -h|--help) sed -n '3,44p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "unknown option: $1" >&2; exit 64 ;;
        *) SOURCE_ROM="$1"; shift ;;
    esac
done

for tool in "$UM80" "$UL80"; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Error: $tool is not on PATH.  pip install um80" >&2
        exit 1
    }
done
[ -f "$SRC_DIR/emu_hbios.asm" ] || {
    echo "Error: $SRC_DIR/emu_hbios.asm not found" >&2; exit 1; }

# The release this tree's bank 0 is cut from.  Not a limit on what the emulator
# can LOAD - that is ROMWBW_SUPPORTED_RELEASES in the same header.
pin() { sed -n "s/^#define ROMWBW_DEFAULT_$1 \([0-9]*\).*/\1/p" "$PIN_H"; }
MAJOR=$(pin MAJOR); MINOR=$(pin MINOR); UPDATE=$(pin UPDATE); PATCH=$(pin PATCH)
[ -n "$MAJOR" ] || { echo "Error: cannot read ROMWBW_DEFAULT_* from $PIN_H" >&2; exit 1; }
if [ "$PATCH" -eq 0 ]; then
    RELEASE="$MAJOR.$MINOR.$UPDATE"
else
    RELEASE="$MAJOR.$MINOR.$UPDATE.$PATCH"
fi
WANT_HCB="57a8$(printf '%x%x%x%x' "$MAJOR" "$MINOR" "$UPDATE" "$PATCH")"

WORK=$(mktemp -d) || exit 1
trap 'rm -rf "$WORK"' EXIT

if [ -z "$OUTPUT" ]; then
    # Not under $WORK: that is removed on exit, and the built ROM is the one
    # thing a caller might want to keep.
    KEEP_DIR=$(mktemp -d) || exit 1
    OUTPUT="$KEEP_DIR/emu-$RELEASE.rom"
else
    # A ROM inside the working tree is what this migration removed.  Refuse it
    # rather than recreate the thing .gitignore now has to catch.
    case "$(cd "$(dirname "$OUTPUT")" 2>/dev/null && pwd)/" in
        "$ROOT"/*|"$ROOT"/)
            echo "Error: $OUTPUT is inside the repository." >&2
            echo "       This repository tracks no ROM images.  Write it" >&2
            echo "       somewhere else, or leave -o off and let this script" >&2
            echo "       pick a temporary directory." >&2
            exit 1 ;;
    esac
fi

echo "Reproducing the RomWBW $RELEASE emulator ROM from source"
echo

# --- what the catalog says this ROM should be ---------------------------------
#
# Read rather than assumed, and keyed on id: nothing here may hardcode
# `emu_avw`, and which upstream ROM supplies banks 1-15 is a property of the
# catalog entry (built_from.banks_1_15), not of this script.

CAT_JSON="$WORK/catalog.json"
have_catalog=0
if ! command -v python3 >/dev/null 2>&1; then
    # Named separately from a network failure.  Both end with the comparison
    # skipped, and telling someone to "re-run with a network" when what is
    # missing is an interpreter sends them to fix the wrong thing.
    echo "  info  python3 is not on PATH, so the catalog cannot be read."
    echo "        Building anyway; the hash comparison at the end - the only"
    echo "        thing that makes this script worth running - will be skipped."
elif [ ! -x "$GET" ]; then
    echo "  info  $GET is missing or not executable, so the catalog cannot"
    echo "        be read.  Building anyway; the hash comparison will be skipped."
elif "$GET" --romwbw "$RELEASE" list --json > "$CAT_JSON" 2>"$WORK/get.err"; then
    have_catalog=1
else
    echo "  info  could not read the catalog for RomWBW $RELEASE:"
    if [ -s "$WORK/get.err" ]; then
        sed 's/^/        /' "$WORK/get.err"
    else
        echo "        (it said nothing)"
    fi
    echo "        Building anyway; the reproducibility check at the end will"
    echo "        be skipped, which is the only thing that makes this script"
    echo "        worth running.  Re-run with a network to get it."
fi

read_entry() {   # $1 = jq-ish field, via python3
    python3 - "$CAT_JSON" "$ROM_ID" "$1" <<'PY' 2>/dev/null || true
import json, sys
doc = json.load(open(sys.argv[1]))
want, field = sys.argv[2], sys.argv[3]
roms = [r for r in doc.get("roms", []) if isinstance(r, dict) and r.get("id")]
ent = None
if want:
    ent = next((r for r in roms if r["id"] == want), None)
else:
    ent = next((r for r in roms if r.get("default")), None) or (roms[0] if roms else None)
if ent is None:
    sys.exit(1)
v = ent
for part in field.split("."):
    v = v.get(part) if isinstance(v, dict) else None
print(v if v is not None else "")
PY
}

if [ "$have_catalog" -eq 1 ]; then
    [ -n "$ROM_ID" ] || ROM_ID=$(read_entry id)
    WANT_SHA=$(read_entry sha256)
    UPSTREAM_ROM=$(read_entry built_from.banks_1_15)
    if [ -z "$ROM_ID" ]; then
        echo "Error: RomWBW $RELEASE publishes no ROM with id '$ROM_ID'" >&2
        exit 1
    fi
    echo "  catalog: $ROM_ID, banks 1-15 from upstream ${UPSTREAM_ROM:-?}"
else
    have_catalog=0
    WANT_SHA=""
    UPSTREAM_ROM=""
fi

# --- banks 1-15 ---------------------------------------------------------------

if [ -z "$SOURCE_ROM" ]; then
    [ -n "$UPSTREAM_ROM" ] || {
        echo "Error: no SOURCE_ROM given and the catalog could not say which" >&2
        echo "       upstream ROM supplies banks 1-15.  Pass one:" >&2
        echo "         roms/build_emu_rom.sh /path/to/SBC_simh_std.rom" >&2
        exit 1; }
    command -v unzip >/dev/null 2>&1 || {
        echo "Error: unzip is needed to take $UPSTREAM_ROM out of the" >&2
        echo "       upstream package.  Install it, or pass a ROM path." >&2
        exit 1; }
    echo "  fetching the upstream RomWBW $RELEASE package (sha256-checked)"
    PKG=$("$GET" --romwbw "$RELEASE" path @upstream) || {
        echo "Error: could not fetch the upstream package" >&2; exit 1; }
    unzip -o -q -j "$PKG" "*/$UPSTREAM_ROM" "$UPSTREAM_ROM" -d "$WORK" 2>/dev/null || true
    SOURCE_ROM="$WORK/$(basename "$UPSTREAM_ROM")"
    [ -f "$SOURCE_ROM" ] || {
        echo "Error: $UPSTREAM_ROM is not in $PKG" >&2; exit 1; }
fi

[ -f "$SOURCE_ROM" ] || { echo "Error: no such file: $SOURCE_ROM" >&2; exit 1; }
SRC_SIZE=$(wc -c < "$SOURCE_ROM" | tr -d ' ')
[ "$SRC_SIZE" -eq 524288 ] || {
    echo "Error: $SOURCE_ROM is $SRC_SIZE bytes; a RomWBW ROM is 524288" >&2
    exit 1; }

# The source ROM must be the release this bank 0 is stamped for.  Its HCB sits
# at 0x103 like ours; a mismatch here is the version-mismatch warning waiting
# to happen, and it is cheaper to refuse than to explain later.
SRC_HCB=$(od -An -tx1 -j 259 -N 4 "$SOURCE_ROM" | tr -d ' \n')
if [ "$SRC_HCB" != "$WANT_HCB" ]; then
    echo "Error: $SOURCE_ROM has HCB $SRC_HCB at 0x103; this tree's" >&2
    echo "       src/emu_hbios.asm is stamped $WANT_HCB (RomWBW $RELEASE)." >&2
    echo "" >&2
    echo "       Overlaying this bank 0 on those banks makes a ROM that boots" >&2
    echo "       and then prints *** WARNING: HBIOS/CBIOS Version Mismatch ***." >&2
    echo "       src/emu_hbios.asm hardcodes its stamp; the parameterised copy" >&2
    echo "       that can build any release lives in romwbw_disks, and" >&2
    echo "       romwbw_disks/tools/build_rom.sh is what builds other releases." >&2
    exit 1
fi

# --- bank 0 -------------------------------------------------------------------

echo "  assembling src/emu_hbios.asm"
# um80 writes its .rel beside the source, so build in a copy: this script must
# not leave anything in src/.
cp "$SRC_DIR/emu_hbios.asm" "$WORK/emu_hbios.asm"
( cd "$WORK" && "$UM80" -g emu_hbios.asm >um80.log 2>&1 ) || {
    echo "Error: emu_hbios.asm does not assemble:" >&2
    sed 's/^/      /' "$WORK/um80.log" >&2
    exit 1; }
"$UL80" -o "$WORK/emu_hbios.bin" -p 0000 "$WORK/emu_hbios.rel" >>"$WORK/um80.log" 2>&1 || {
    echo "Error: emu_hbios.rel does not link:" >&2
    sed 's/^/      /' "$WORK/um80.log" >&2
    exit 1; }

B0=$(wc -c < "$WORK/emu_hbios.bin" | tr -d ' ')
[ "$B0" -le 32768 ] || {
    echo "Error: bank 0 assembled to $B0 bytes; it must fit in 32768" >&2; exit 1; }
echo "        bank 0 is $B0 bytes of 32768"

dd if=/dev/zero bs=32768 count=1 of="$WORK/bank0.bin" 2>/dev/null
dd if="$WORK/emu_hbios.bin" of="$WORK/bank0.bin" conv=notrunc 2>/dev/null

# --- the overlay --------------------------------------------------------------

mkdir -p "$(dirname "$OUTPUT")"
cp "$SOURCE_ROM" "$OUTPUT"
chmod u+w "$OUTPUT"
dd if="$WORK/bank0.bin" of="$OUTPUT" bs=32768 count=1 conv=notrunc 2>/dev/null

BUILT_HCB=$(od -An -tx1 -j 259 -N 4 "$OUTPUT" | tr -d ' \n')
if [ "$BUILT_HCB" != "$WANT_HCB" ]; then
    echo "Error: the ROM just built has HCB $BUILT_HCB at 0x103, expected" >&2
    echo "       $WANT_HCB.  Removing it so a broken image is not left behind." >&2
    rm -f "$OUTPUT"
    exit 1
fi

echo
echo "Built $OUTPUT ($(wc -c < "$OUTPUT" | tr -d ' ') bytes), HCB $BUILT_HCB"

# --- the point of the exercise ------------------------------------------------

if [ -z "$WANT_SHA" ]; then
    echo
    echo "info  no published hash to compare against, so this run proves"
    echo "      nothing beyond 'it assembles'.  Run it with a network."
    exit 0
fi

if command -v sha256sum >/dev/null 2>&1; then
    GOT=$(sha256sum "$OUTPUT" | cut -d' ' -f1)
else
    GOT=$(shasum -a 256 "$OUTPUT" | cut -d' ' -f1)
fi
echo
if [ "$GOT" = "$WANT_SHA" ]; then
    echo "PASS: byte-identical to the published $ROM_ID for RomWBW $RELEASE"
    echo "      sha256 $GOT"
    echo "      The ROM users download is reproducible from the source in this"
    echo "      repository.  That is what docs/ROM_ATTESTATION.md asserts."
    exit 0
fi
echo "FAIL: this build does NOT match the published $ROM_ID for RomWBW $RELEASE"
echo "      built     $GOT"
echo "      published $WANT_SHA"
echo
echo "      Either src/emu_hbios.asm here has drifted from the copy"
echo "      romwbw_disks builds with, or the published ROM was cut from a"
echo "      different upstream package.  romwbw_disks'"
echo "      tools/check_source_drift.sh compares the two trees' sources."
exit 1
