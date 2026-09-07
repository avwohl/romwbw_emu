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

# -T logical, when cpmtools has it, and why it is not optional here.
#
# cpmtools 2.23 cannot be configured without libdsk, and its default libdsk
# path DOUBLE-COUNTS boottrk on a diskdef that carries no `offset`.  Measured
# 2026-09-07 against the published hd1k_infocom, whose directory is at
# boottrk*sectrk*seclen = 16384:
#
#   cpmls -f wbw_hd1k             lists nothing at all
#   cpmcp -f wbw_hd1k ... 0:f.txt exits 0 and writes at 32768 - exactly 16384
#                                 too far, into the data area
#   cpmcp -T logical -f wbw_hd1k  writes at 23233, inside the directory
#
# So the bare form does not merely fail to read: it silently corrupts an image
# it is asked to write to, and then a bare cpmls reads the corruption back as a
# plausible directory.  `-T logical` bypasses the auto-probe and both read and
# write land where the geometry says.  The offset-carrying slice definitions
# are unaffected either way, which is why hd1k_combo always looked fine.
TFLAG=""
if cpmls 2>&1 | grep -q '\-T'; then
    TFLAG="-T logical"
fi

# Which diskdef an image wants, from its size.  Exactly 8 MB is a plain
# single-slice hd1k; anything larger is a combo whose slice 0 sits behind a
# 1 MB MBR prefix.  Same rule the emulator's own auto-detect uses
# (docs/DISK_FORMATS.md), so a disagreement here is a disagreement there.
diskdef_for() {
    case "$(filesize "$1")" in
        8388608) echo wbw_hd1k ;;
        *)       echo wbw_hd1k_0 ;;
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

if ! command -v cpmcp >/dev/null 2>&1; then
    note "skip  cpmtools is not on PATH - cannot look inside an image"
    note "      (the source gate above does not need it)"
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
            def=$(diskdef_for "$path")
            base=$(basename "$path")
            # DID THIS DISKDEF READ THE IMAGE AT ALL?  Asked before anything
            # is concluded from the listing, because the way a wrong diskdef
            # fails is the reason this file exists: cpmls does not error, it
            # prints a directory that is empty or is mojibake, and both read as
            # "the utility is not on this image".
            #
            # Measured on macOS with homebrew cpmtools 2.23 (linked against
            # libdsk 3), against images that demonstrably contain files - the
            # directory entries are visible with od at boottrk*sectrk*seclen:
            #
            #   hd1k_infocom  wbw_hd1k -> an empty listing
            #   hd1k_games    wbw_hd1k -> 711 bytes of mojibake
            #   the same bytes behind a 512-byte prefix, read through an
            #   otherwise identical definition carrying `offset 512` -> correct
            #
            # An explicit `offset 0` does not help, so it is the presence of a
            # nonzero offset that matters, not the value.  wbw_hd1k has no
            # offset line, which is why the combo slices (wbw_hd1k_0, offset
            # 1048576) read here and the plain images do not.  On the Ubuntu
            # runner this same definition worked, so it is a property of the
            # cpmtools build rather than of the definition or the image.
            #
            # A note rather than a failure: a machine whose cpmtools cannot
            # open an image has not found anything wrong with the image. What
            # it must not do is go quiet and let the run report a pass.
            listing=$( ( cd "$SELF_ROOT/disks" && cpmls $TFLAG -f "$def" "$path" ) \
                         2>/dev/null | sed 's/^[0-9]*://' )
            names=$(printf '%s' "$listing" | tr -d ' \t\n')
            # CP/M 2.2 allows letters, digits and a small punctuation set in a
            # filename; anything else on the line means the bytes being read as
            # a directory are not one.
            if [ -z "$names" ] || printf '%s' "$names" | LC_ALL=C grep -q '[^A-Za-z0-9._$!#%&()@~^{}+-]'; then
                if [ -z "$names" ]; then
                    note "note  $base: diskdef $def lists an EMPTY directory"
                else
                    note "note  $base: diskdef $def lists a GARBAGE directory"
                fi
                note "      so nothing was compared on this image.  Either it is"
                note "      blank, or this cpmtools cannot read it: a libdsk-linked"
                note "      build returns nothing usable for a diskdef with no"
                note "      offset, which is what $def is.  A device_posix build"
                note "      of the same cpmtools sources reads them."
                continue
            fi
            for util in $built; do
                # Every cpmtools command runs from disks/, which carries its own
                # diskdefs.  cpmtools reads ./diskdefs if there is one and the
                # system file otherwise, and not every distribution's system file
                # has the combo slice definitions - Debian and Ubuntu's cpmtools
                # 2.23 has wbw_hd1k but not wbw_hd1k_0, on which this check once
                # reported hd1k_combo.img as holding neither utility.
                if ! ( cd "$SELF_ROOT/disks" && cpmls $TFLAG -f "$def" "$path" ) 2>/dev/null |
                        grep -qi "^$util\.com$"; then
                    # Not a failure any more.  This repository used to ship two
                    # images that were known to carry both utilities, so a
                    # missing copy could only mean damage.  Now the images come
                    # from a catalog that publishes twenty-odd of them and marks
                    # the ones carrying host transfer with `host_transfer`; the
                    # other twenty are supposed not to have it.
                    continue
                fi
                if ! ( cd "$SELF_ROOT/disks" && \
                       cpmcp $TFLAG -f "$def" "$path" "0:$util.com" "$TMP/from_img.com" ) 2>/dev/null; then
                    bad "$base $util.com" "could not be extracted (diskdef $def?)"
                    continue
                fi
                if [ "$util" = w8 ]; then
                    if has_caps_interlock "$TMP/from_img.com"; then
                        interlocked=$((interlocked + 1))
                        ok "$base w8.com" "asks HBF_HOST_CAPS before using a host path"
                    else
                        bad "$base w8.com" "hands over a host path with no interlock"
                        note "      This image carries a W8 from before v1.36.  It is not"
                        note "      one this repository can fix - re-fetch it, and if the"
                        note "      published image really is that old, say so upstream:"
                        note "      romwbw-get verify --repair"
                    fi
                fi
                if is_zero_padded "$TMP/from_img.com"; then
                    bad "$base $util.com" "is a padded memory image, not a bare .COM"
                    note "      Every address constant differs by 0x100, so cmp cannot"
                    note "      speak.  This image was built by a tree with an ORG in"
                    note "      src/$util.asm."
                    continue
                fi
                checked=$((checked + 1))
                if cmp -s "$TMP/$util.com" "$TMP/from_img.com"; then
                    ok "$base $util.com" "matches src/$util.asm"
                else
                    b=$(filesize "$TMP/$util.com")
                    h=$(filesize "$TMP/from_img.com")
                    bad "$base $util.com" \
                        "differs from src/$util.asm (built $b bytes, image holds $h)"
                    note "      The published images are built by romwbw_disks from its"
                    note "      own copy of these sources; a difference here means the"
                    note "      two trees have drifted.  romwbw_disks'"
                    note "      tools/check_source_drift.sh is the check for that."
                fi
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
