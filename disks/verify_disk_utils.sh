#!/bin/sh
#
# Are r8.com and w8.com still what src/r8.asm and src/w8.asm build, and does
# w8 still ask permission before it hands over a host path?
#
# Two checks, and they need different things:
#
#   THE SOURCE GATE needs nothing but um80 and ul80.  It assembles both
#   utilities and asserts that the w8.com they produce still contains the
#   HBF_HOST_CAPS interlock.  This runs on every machine that can build the
#   project, with no network and no disk image, and it is the only thing
#   standing between an edit to src/w8.asm and a W8 that hands a guest-supplied
#   host path to a front end that has promised nothing about where it lands.
#
#   THE IMAGE CHECK needs images, and this repository no longer has any.  The
#   ROMs and disk images moved to avwohl/romwbw_disks, which builds and
#   verifies the published ones itself (tools/build_utils.sh asserts the same
#   interlock at build time, tools/verify_catalog.py asserts it against every
#   published image that carries w8.com).  What is left here is the ability to
#   ask the same question of images that are actually on this machine - a
#   romwbw-get cache, or any directory named on the command line - which is
#   what turns "the catalog says these bytes" into "the bytes I downloaded".
#
# Usage: disks/verify_disk_utils.sh [image-dir ...]
#
#   With no argument it looks in the romwbw-get cache - $ROMWBW_GET_CACHE, else
#   $XDG_CACHE_HOME/romwbw_emu, else ~/.cache/romwbw_emu - and checks whatever
#   is there, skipping .superseded and .partial.  An empty cache is a SKIP of
#   the image half, not a failure: a machine that has never fetched anything
#   has nothing to be wrong.
#
# Exit:  0 the source gate passed, and every image found matched
#        1 the source gate failed, or an image holds something else
#
# Requires um80 + ul80 (the MACRO-80 toolchain this project's .asm files are
# written for - pip install um80).  cpmtools is needed only for the image half.
# None are build dependencies of the emulator, so a machine without them skips.
#
# The diskdef is per image and getting it wrong does NOT fail loudly: cpmls with
# the wrong one prints a garbage directory, which reads as "no such file".  That
# is exactly how hd1k_infocom.img was once recorded as carrying no w8.com when
# it carries both.  Hence the explicit size-to-diskdef mapping below rather than
# one default.
#
# On the interlock, and why it is on bytes: nothing in w8.com's text
# discriminates.  The armed 1408-byte build prints the same
# "Usage: W8 <cpmname> [hostpath]" as the 1792-byte interlocked one, so the
# assertion is on the instruction bytes `06 e9 cf` - `ld b,H_CAPS` then `rst 8`,
# which is the whole of w8.asm's check_host_path_safe.

set -u

SELF_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$SELF_ROOT"

fail=0
checked=0
interlocked=0
images_seen=0

note()  { printf '  %s\n' "$*"; }
ok()    { printf 'ok    %-30s %s\n' "$1" "$2"; }
bad()   { printf 'FAIL  %-30s %s\n' "$1" "$2"; fail=$((fail + 1)); }

# True when the first 256 bytes of $1 are all zero, i.e. the file is a ul80
# memory image rather than a bare .COM - the layout both sources used to build
# in.  Nothing should be in it now; a hit means someone put an ORG back, so this
# says so rather than reporting the 0x100 shift in every address as drift.  od
# rather than tr: the input is binary and macOS tr rejects it outright with
# "Illegal byte sequence".
is_zero_padded() {
    [ "$(wc -c < "$1" | tr -d ' ')" -gt 256 ] || return 1
    # No {512} repetition count: BSD grep caps repetition at 255.  "contains no
    # character other than 0" says the same thing and is portable.
    ! head -c 256 "$1" | od -An -v -tx1 | tr -d ' \n' | grep -q '[^0]'
}

# Bytes, via od, rather than `xxd -p | grep`: xxd is not on a stock Ubuntu
# runner (it ships with vim-common), and a flat hex string also matches across a
# byte boundary - 0xB0 0x6E 0x9C 0xF3 reads as "b06e9cf3" and would answer yes.
# The spaces od leaves between bytes are what keeps the match aligned, so the
# newlines become spaces rather than being deleted.
has_caps_interlock() {
    od -An -v -tx1 "$1" | tr '\n' ' ' | tr -s ' ' | grep -q ' 06 e9 cf '
}

filesize() { wc -c < "$1" | tr -d ' '; }

# HOW AN IMAGE IS READ: cpm_disk.py, and not cpmtools.
#
# cpm_disk.py is the family's CP/M image tool - one stdlib-only Python file,
# owned by cpmemu (util/cpm_disk.py) and used by every repository here.  It
# needs no diskdefs, has no -T logical, and is not linked against libdsk, so
# the whole class of hazard this file used to carry is gone with it:
#
#   - cpmtools 2.23 cannot be configured without libdsk, and its default path
#     DOUBLE-COUNTS boottrk on a diskdef with no `offset`.  Measured against
#     the published hd1k_infocom, whose directory is at 16384: `cpmls -f
#     wbw_hd1k` listed nothing, and `cpmcp` exited 0 having written at 32768 -
#     into the data area, over a file that was already there.  It did not fail;
#     it corrupted the image and then read the corruption back as a plausible
#     directory.
#   - Which diskdef an image wants had to be guessed from its size here, and
#     guessing wrong printed a garbage directory rather than an error - which
#     reads as "the utility is not on this image".  That is how hd1k_infocom
#     was once recorded as carrying no w8.com when it carries both.
#   - libdsk cannot address past 8 MB from the start of a file, so slices 1-5
#     of a combo were unreachable and slice 0's last 1 MB was too.
#
# cpm_disk.py auto-detects hd1k from combo by size, addresses any slice with
# --slice N, and reads the whole file.  So this check now looks at EVERY slice
# of a combo instead of only the first.  Measured 2026-09-10 against the
# published hd1k_combo: R8.COM and W8.COM are on slice 0 and on no other, which
# is what the catalog's host_transfer flag says.
#
# Found the same way src/makefile finds qkz80: an explicit variable, then a
# sister checkout, then whatever `make install` put on PATH.  $CPM_DISK is
# spelled to match the mpm2 repository, which already drives this tool that way.
CPM_DISK="${CPM_DISK:-}"
if [ -z "$CPM_DISK" ]; then
    for cand in \
        "$SELF_ROOT/../cpmemu/util/cpm_disk.py" \
        "$SELF_ROOT/cpmemu/util/cpm_disk.py"
    do
        [ -f "$cand" ] && { CPM_DISK="$cand"; break; }
    done
fi

# How to run it: a path needs an interpreter, an installed `cpm_disk` does not.
#
# NOT named cpm_disk.  A shell function shadows the command of the same name in
# `command -v`, so have_cpm_disk() answered yes on a machine with neither, the
# skip below never fired, and the run reported "0 disk-resident binaries
# checked" as a PASS - a green check that had opened nothing, which is the one
# outcome this file exists to prevent.
run_cpm_disk() {
    if [ -n "$CPM_DISK" ]; then
        python3 "$CPM_DISK" "$@"
    else
        cpm_disk "$@"
    fi
}

have_cpm_disk() {
    [ -n "$CPM_DISK" ] && return 0
    command -v cpm_disk >/dev/null 2>&1
}

# How many slices to look at.  Exactly 8 MB is a plain single-slice hd1k;
# a combo is a 1 MB MBR prefix plus six of them.  Same rule the emulator's own
# auto-detect uses (docs/DISK_FORMATS.md), so a disagreement here is a
# disagreement there.
slices_of() {
    case "$(filesize "$1")" in
        8388608) echo "" ;;          # plain image: no --slice at all
        *)       echo "0 1 2 3 4 5" ;;
    esac
}

UTILS="r8 w8"

for tool in um80 ul80; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "SKIP: $tool is not on PATH - cannot build the disk utilities"
        echo "      (pip install um80)"
        exit 0
    fi
done

TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT

# --- the source gate: no network, no image, no cpmtools ----------------------

echo "From source:"
built=""
for util in $UTILS; do
    src="$ROOT/src/$util.asm"
    if [ ! -f "$src" ]; then
        bad "src/$util.asm" "is missing"
        continue
    fi
    if ! um80 -o "$TMP/$util.rel" "$src" >"$TMP/$util.log" 2>&1; then
        bad "src/$util.asm" "does not assemble - see below"
        sed 's/^/      /' "$TMP/$util.log"
        continue
    fi
    if ! ul80 -o "$TMP/$util.com" "$TMP/$util.rel" >>"$TMP/$util.log" 2>&1; then
        bad "src/$util.asm" "assembles but does not link - see below"
        sed 's/^/      /' "$TMP/$util.log"
        continue
    fi
    if is_zero_padded "$TMP/$util.com"; then
        bad "src/$util.asm" "linked with 256 leading zero bytes"
        note "      L80 bases a .COM at 0100h already, and an ORG on top of that"
        note "      puts the code at 0200h behind a NOP pad.  It runs, and it"
        note "      makes the binary incomparable with everything else.  Remove"
        note "      the \`org 0100h\` from src/$util.asm."
        continue
    fi
    built="$built $util"
    ok "src/$util.asm" "builds a bare .COM ($(filesize "$TMP/$util.com") bytes)"
done

case " $built " in
    *" w8 "*)
        if has_caps_interlock "$TMP/w8.com"; then
            ok "src/w8.asm" "asks HBF_HOST_CAPS before using a host path"
        else
            bad "src/w8.asm" "builds a W8 with no host-path interlock"
            note "      The bytes 06 e9 cf - \`ld b,H_CAPS\` then \`rst 8\` - are not"
            note "      in what src/w8.asm just built, so this W8 passes a"
            note "      guest-supplied host path to whatever emulator opens the"
            note "      image, and that emulator may have made no promise about"
            note "      where the path can reach.  The usage message does not"
            note "      tell the two apart, which is why this is a byte check."
        fi
        ;;
esac

# --- the image half: whatever is on this machine ------------------------------

if [ "$#" -gt 0 ]; then
    DIRS="$*"
    explicit=1
else
    # ROMWBW_GET_CACHE first, exactly as tools/romwbw-get's cache_root() and
    # roms/verify_romwbw_pin.sh both resolve it.  Without it, a machine that
    # sets that variable sends this script to look in an empty directory: it
    # prints "no .img" and passes, which is the inspected-nothing failure the
    # header above says this file exists to prevent.
    DIRS="${ROMWBW_GET_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/romwbw_emu}"
    explicit=0
fi

echo
echo "Images on this machine:"

if ! have_cpm_disk; then
    note "skip  cpm_disk.py not found - cannot look inside an image"
    note "      (the source gate above does not need it).  Set CPM_DISK, or put"
    note "      a cpmemu checkout beside this one, or run cpmemu's make install."
else
    for dir in $DIRS; do
        if [ ! -d "$dir" ]; then
            [ "$explicit" -eq 1 ] && bad "$dir" "is not a directory"
            continue
        fi
        # find, not a glob: the romwbw-get cache nests images under
        # v0/<release>/assets/, and a bare *.img would silently match nothing.
        #
        # .superseded and .partial are pruned. The first holds images a
        # generation bump replaced - deliberately kept, deliberately not
        # current - and checking them would report drift against sources that
        # have moved on. The second holds downloads in flight.
        for path in $(find "$dir" \( -name .superseded -o -name .partial \) -prune -o \
                           -type f -name '*.img' -print 2>/dev/null | sort); do
            [ -f "$path" ] || continue
            images_seen=$((images_seen + 1))
            base=$(basename "$path")
            for sl in $(slices_of "$path") ""; do
                # The loop always ends with an empty element so a plain image
                # runs exactly once with no --slice; for a combo the empty
                # element is skipped, because slice 0 was already done.
                if [ -z "$sl" ]; then
                    [ "$(filesize "$path")" = 8388608 ] || continue
                    sflag=""; label="$base"
                else
                    sflag="--slice $sl"; label="$base slice $sl"
                fi

                listing=$(run_cpm_disk list $sflag "$path" 2>/dev/null | sed 1,2d)
                if [ -z "$listing" ]; then
                    # Not a failure: an empty slice is a normal thing for a
                    # combo to contain, and cpm_disk.py needs no diskdef to
                    # get wrong, so "empty" here means empty.
                    continue
                fi
                for util in $built; do
                    if ! printf '%s\n' "$listing" |
                            awk '{print $2}' | grep -qi "^$util\.com$"; then
                        # The catalog publishes twenty-odd images and marks the
                        # ones carrying host transfer with `host_transfer`; the
                        # rest are supposed not to have it.
                        continue
                    fi
                    rm -rf "$TMP/img" "$TMP/from_img.com"
                    mkdir -p "$TMP/img" || exit 1
                    if ! run_cpm_disk extract $sflag "$path" "$(echo "$util" | tr a-z A-Z).COM" \
                            -o "$TMP/img" >/dev/null 2>&1; then
                        bad "$label $util.com" "could not be extracted"
                        continue
                    fi
                    mv "$TMP/img/$util.com" "$TMP/from_img.com" 2>/dev/null || {
                        bad "$label $util.com" "extracted to an unexpected name"
                        continue
                    }
                    if [ "$util" = w8 ]; then
                        if has_caps_interlock "$TMP/from_img.com"; then
                            interlocked=$((interlocked + 1))
                            ok "$label w8.com" "asks HBF_HOST_CAPS before using a host path"
                        else
                            bad "$label w8.com" "hands over a host path with no interlock"
                            note "      This image carries a W8 from before v1.36.  It is not"
                            note "      one this repository can fix - re-fetch it, and if the"
                            note "      published image really is that old, say so upstream:"
                            note "      romwbw-get verify --repair"
                        fi
                    fi
                    if is_zero_padded "$TMP/from_img.com"; then
                        bad "$label $util.com" "is a padded memory image, not a bare .COM"
                        note "      Every address constant differs by 0x100, so cmp cannot"
                        note "      speak.  This image was built by a tree with an ORG in"
                        note "      src/$util.asm."
                        continue
                    fi
                    checked=$((checked + 1))
                    if cmp -s "$TMP/$util.com" "$TMP/from_img.com"; then
                        ok "$label $util.com" "matches src/$util.asm"
                    else
                        b=$(filesize "$TMP/$util.com")
                        h=$(filesize "$TMP/from_img.com")
                        bad "$label $util.com" \
                            "differs from src/$util.asm (built $b bytes, image holds $h)"
                        note "      The published images are built by romwbw_disks from its"
                        note "      own copy of these sources; a difference here means the"
                        note "      two trees have drifted.  romwbw_disks'"
                        note "      tools/check_source_drift.sh is the check for that."
                    fi
                done
            done
        done
    done

    if [ "$images_seen" -eq 0 ]; then
        note "skip  no .img under $DIRS"
        if [ "$explicit" -eq 0 ]; then
            note "      Nothing has been fetched on this machine yet.  This is the"
            note "      expected state of a fresh clone since v1.40: the images are"
            note "      published at github.com/avwohl/romwbw_disks, not tracked"
            note "      here.  Populate the cache with:  tools/romwbw-get fetch"
        fi
    fi
fi

echo
if [ "$fail" -ne 0 ]; then
    echo "FAIL: $fail check(s) failed on the disk utilities"
    exit 1
fi
echo "PASS: src/r8.asm and src/w8.asm build, and w8 carries the HBF_HOST_CAPS interlock"
echo "PASS: $images_seen image(s) inspected, $checked disk-resident binar$( [ "$checked" -eq 1 ] && echo y || echo ies ) checked, $interlocked interlock(s) confirmed"
exit 0
