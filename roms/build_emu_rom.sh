#!/bin/sh
#
# build_emu_rom.sh - DEBUGGING TOOL.  Reproduce a published ROM locally and
# prove it is bit-for-bit what romwbw_disks publishes.
#
# DO NOT RUN THIS IN THE NORMAL COURSE OF WORK.  ROMs are entirely
# romwbw_disks' province: its tools/build_rom.sh cuts every ROM users download,
# from its own byte-identical copy of src/emu_hbios.asm.  Nothing in this
# repository's build, test or release path assembles a ROM, and nothing should
# start - romwbw_disks/tools/check_source_drift.sh is what continuously asserts
# the two trees agree, and it needs no ROM at all.
#
# Run this when something is actually WRONG: a suspected drift between the two
# copies of bank 0, or a check of the reproducibility claim
# docs/ROM_ATTESTATION.md and the GPL both turn on.  Its real output is not the
# file - it is the hash comparison at the end.
#
#   bank 0      32 KB, assembled here from src/emu_hbios.asm - the HBIOS proxy
#   banks 1-15 480 KB, lifted verbatim from an upstream RomWBW ROM
#
# Usage: roms/build_emu_rom.sh [--romwbw VER] [-o OUTPUT] [--rom-id ID] [SOURCE_ROM]
#
#   --romwbw VER which RomWBW release to reproduce.  With none given, the
#                release is taken from SOURCE_ROM's own HCB if you named one,
#                and otherwise from the catalog's default.
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
# THERE IS NO RELEASE HARDCODED HERE, and that is the point.  Bank 0's version
# stamp is generated into romwbw_ver.inc from the HCB of the stock ROM being
# overlaid, so bank 0 and banks 1-15 name the same release BY CONSTRUCTION and
# the guest's "HBIOS/CBIOS Version Mismatch" warning cannot be provoked by a
# mistake in this script.  src/emu_hbios.asm is byte-identical to the copy
# romwbw_disks builds the published ROMs with, and its tools/build_rom.sh
# generates the same include from versions/<ver>/version.json.  Until 2026-09-17
# this script could only build the one release a ROMWBW_DEFAULT_* macro named,
# which by then was not even the catalog's default.
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
GET="$ROOT/tools/romwbw-get"

UM80="${UM80:-um80}"
UL80="${UL80:-ul80}"

OUTPUT=""
ROM_ID=""
SOURCE_ROM=""
RELEASE=""

while [ $# -gt 0 ]; do
    case "$1" in
        -o) [ $# -ge 2 ] || { echo "-o needs a path" >&2; exit 64; }
            OUTPUT="$2"; shift 2 ;;
        -o*) OUTPUT="${1#-o}"; shift ;;
        --rom-id) [ $# -ge 2 ] || { echo "--rom-id needs an id" >&2; exit 64; }
            ROM_ID="$2"; shift 2 ;;
        --rom-id=*) ROM_ID="${1#--rom-id=}"; shift ;;
        --romwbw) [ $# -ge 2 ] || { echo "--romwbw needs a version" >&2; exit 64; }
            RELEASE="$2"; shift 2 ;;
        --romwbw=*) RELEASE="${1#--romwbw=}"; shift ;;
        # The whole header block, whatever length it grows to: a fixed range
        # silently truncated --help when the header was rewritten.
        -h|--help) sed -n '3,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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

WORK=$(mktemp -d) || exit 1
trap 'rm -rf "$WORK"' EXIT

# The HCB sits at 0x100; its version bytes are at 0x105/0x106, i.e. 261/262
# from the start of the file.  Reading them is how this script learns which
# release it is building, rather than being told by a macro.
hcb_bytes() {   # $1 = rom file, $2 = skip, $3 = count -> lowercase hex, no spaces
    od -An -tx1 -j "$2" -N "$3" "$1" | tr -d ' \n'
}

release_of_rom() {   # $1 = rom file -> dotted release, or empty
    _m=$(hcb_bytes "$1" 259 2)
    [ "$_m" = "57a8" ] || return 1
    _v=$(hcb_bytes "$1" 261 1)
    _u=$(hcb_bytes "$1" 262 1)
    _maj=$((0x$_v >> 4)); _min=$((0x$_v & 15))
    _upd=$((0x$_u >> 4)); _pat=$((0x$_u & 15))
    if [ "$_pat" -eq 0 ]; then
        printf '%d.%d.%d' "$_maj" "$_min" "$_upd"
    else
        printf '%d.%d.%d.%d' "$_maj" "$_min" "$_upd" "$_pat"
    fi
}

# A SOURCE_ROM names its own release, so honour it rather than asking the
# catalog for a default the caller did not want.
if [ -z "$RELEASE" ] && [ -n "$SOURCE_ROM" ]; then
    [ -f "$SOURCE_ROM" ] || { echo "Error: no such file: $SOURCE_ROM" >&2; exit 1; }
    RELEASE=$(release_of_rom "$SOURCE_ROM" || true)
    [ -n "$RELEASE" ] || {
        echo "Error: $SOURCE_ROM has no HBIOS configuration block at 0x103," >&2
        echo "       so this script cannot tell which release it is.  Name it:" >&2
        echo "         roms/build_emu_rom.sh --romwbw 3.6.0 $SOURCE_ROM" >&2
        exit 1; }
fi

# --- what the catalog says this ROM should be ---------------------------------
#
# Read rather than assumed, and keyed on id: nothing here may hardcode
# `emu_avw`, and which upstream ROM supplies banks 1-15 is a property of the
# catalog entry (built_from.banks_1_15), not of this script.

CAT_JSON="$WORK/catalog.json"
have_catalog=0
set -- list --json
[ -n "$RELEASE" ] && set -- --romwbw "$RELEASE" "$@"
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
elif "$GET" "$@" > "$CAT_JSON" 2>"$WORK/get.err"; then
    have_catalog=1
else
    echo "  info  could not read the catalog${RELEASE:+ for RomWBW $RELEASE}:"
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
if field == "@__ids__":
    print(", ".join(r["id"] for r in roms))
    sys.exit(0)
if field.startswith("@"):
    print(doc.get(field[1:], "") or "")
    sys.exit(0)
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
    # The catalog resolved the default release when we did not name one, so
    # this is where an unspecified build learns what it is building.
    [ -n "$RELEASE" ] || RELEASE=$(read_entry '@romwbw_version')
    ASKED_ID="$ROM_ID"
    [ -n "$ROM_ID" ] || ROM_ID=$(read_entry id)
    WANT_SHA=$(read_entry sha256)
    UPSTREAM_ROM=$(read_entry built_from.banks_1_15)
    # A --rom-id that matches nothing must be NAMED.  read_entry swallows its
    # own failure (|| true), so without this the run would build a ROM, skip
    # the hash comparison that is the whole point, and blame the network.
    if [ -z "$ROM_ID" ] || { [ -n "$ASKED_ID" ] && [ -z "$WANT_SHA" ]; }; then
        echo "Error: RomWBW $RELEASE publishes no ROM with id '${ASKED_ID:-$ROM_ID}'." >&2
        echo "       It publishes: $(read_entry '@__ids__')" >&2
        exit 1
    fi
    echo "Reproducing the RomWBW $RELEASE emulator ROM from source"
    echo
    echo "  catalog: $ROM_ID, banks 1-15 from upstream ${UPSTREAM_ROM:-?}"
else
    WANT_SHA=""
    UPSTREAM_ROM=""
    echo "Building the RomWBW ${RELEASE:-?} emulator ROM from source"
    echo
fi

if [ -z "$OUTPUT" ]; then
    # Not under $WORK: that is removed on exit, and the built ROM is the one
    # thing a caller might want to keep.
    KEEP_DIR=$(mktemp -d) || exit 1
    OUTPUT="$KEEP_DIR/emu-${RELEASE:-unknown}.rom"
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

# Banks 1-15 carry a CBIOS built for one release, and bank 0 is about to be
# stamped to match them.  If the caller ALSO named a release, the two have to
# agree, or the ROM would boot and print
# "*** WARNING: HBIOS/CBIOS Version Mismatch ***".
SRC_RELEASE=$(release_of_rom "$SOURCE_ROM" || true)
[ -n "$SRC_RELEASE" ] || {
    echo "Error: $SOURCE_ROM has no HBIOS configuration block at 0x103 -" >&2
    echo "       it is not a RomWBW ROM, or it is corrupt." >&2
    exit 1; }
if [ -n "$RELEASE" ] && [ "$SRC_RELEASE" != "$RELEASE" ]; then
    echo "Error: $SOURCE_ROM is a RomWBW $SRC_RELEASE ROM, but this build is" >&2
    echo "       for $RELEASE.  Banks 1-15 and bank 0 must be the same" >&2
    echo "       release, or the guest prints" >&2
    echo "       *** WARNING: HBIOS/CBIOS Version Mismatch ***." >&2
    exit 1
fi
RELEASE="$SRC_RELEASE"
WANT_HCB=$(hcb_bytes "$SOURCE_ROM" 259 4)

# --- bank 0 -------------------------------------------------------------------

echo "  assembling src/emu_hbios.asm for RomWBW $RELEASE"
# um80 writes its .rel beside the source, and resolves `include` relative to
# the working directory, so the source and the generated include are assembled
# together in a copy: this script must not leave anything in src/.
cp "$SRC_DIR/emu_hbios.asm" "$WORK/emu_hbios.asm"
{
    echo "; GENERATED by roms/build_emu_rom.sh from $(basename "$SOURCE_ROM")'s"
    echo "; HBIOS configuration block - do not edit.  RomWBW v$RELEASE."
    printf 'RMV_VER\tequ\t0%sh\n' "$(hcb_bytes "$SOURCE_ROM" 261 1)"
    printf 'RMV_UPD\tequ\t0%sh\n' "$(hcb_bytes "$SOURCE_ROM" 262 1)"
} > "$WORK/romwbw_ver.inc"

( cd "$WORK" && "$UM80" -g emu_hbios.asm >um80.log 2>&1 ) || {
    echo "Error: emu_hbios.asm does not assemble:" >&2
    sed 's/^/      /' "$WORK/um80.log" >&2
    exit 1; }
( cd "$WORK" && "$UL80" -o emu_hbios.bin -p 0000 emu_hbios.rel >>um80.log 2>&1 ) || {
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

BUILT_HCB=$(hcb_bytes "$OUTPUT" 259 4)
if [ "$BUILT_HCB" != "$WANT_HCB" ]; then
    echo "Error: the ROM just built has HCB $BUILT_HCB at 0x103, but banks" >&2
    echo "       1-15 say $WANT_HCB.  The generated romwbw_ver.inc did not" >&2
    echo "       reach CB_VERSION.  Removing it so a broken image is not" >&2
    echo "       left behind." >&2
    rm -f "$OUTPUT"
    exit 1
fi

echo
echo "Built $OUTPUT ($(wc -c < "$OUTPUT" | tr -d ' ') bytes), HCB $BUILT_HCB"

# --- the point of the exercise ------------------------------------------------

if [ -z "$WANT_SHA" ]; then
    echo
    echo "info  no published hash to compare against, so this run proves"
    echo "      nothing beyond 'it assembles'."
    if command -v python3 >/dev/null 2>&1; then
        echo "      Re-run with a network to get it."
    else
        echo "      Install python3 to get it - the catalog is read through"
        echo "      tools/romwbw-get, which is a python3 script."
    fi
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
