#!/bin/sh
#
# Verify that the R8/W8 binaries inside the tracked disk images are what
# src/r8.asm and src/w8.asm currently assemble to.
#
# Nothing builds these .COM files as part of any target: they were assembled by
# hand once and copied into the images, so the source and the shipped binary can
# drift with nothing to notice.  The last drift - a stale w8.com in
# hd1k_infocom.img - was found by hand, and only after the source had already
# changed underneath it.  This turns that into a check.
#
# What it does, per image and per utility: assemble the source, link it, extract
# the copy the image holds, and compare.
#
# What it found on its first run is worth knowing before reading its output: the
# two utilities in these images were not built the same way.  w8.com is a ul80
# memory image - 256 leading NOPs, code at file offset 0x100, every address
# constant 256 higher to suit - while r8.com is a bare .COM with the code at
# offset 0.  Both run; CP/M loads either at 0100h and the NOPs simply slide into
# the code.  But only a same-layout pair can be compared byte for byte, so this
# reports r8 as not comparable rather than pretending a 0x100 offset in every
# address is evidence of drift.
#
# Usage: disks/verify_disk_utils.sh [tree_root]
# Exit:  0 all present copies match (or the tools to check are missing)
#        1 at least one image holds a binary that is not what the source builds
#
# Requires um80 + ul80 (the MACRO-80 toolchain this project's .asm files are
# written for - pip install um80) and cpmtools with a diskdefs carrying the
# wbw_hd1k family.  None of these are build dependencies of the emulator, so a
# machine without them skips rather than failing.
#
# The diskdef is per image and getting it wrong does NOT fail loudly: cpmls with
# the wrong one prints a garbage directory, which reads as "no such file".  That
# is exactly how hd1k_infocom.img was once recorded as carrying no w8.com when
# it carries both.  Hence the explicit table below rather than one default.

set -u

SELF_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="${1:-$SELF_ROOT}"

fail=0
skip=0
checked=0

note()  { printf '  %s\n' "$*"; }

# True when the first 256 bytes of $1 are all zero, i.e. the file is a ul80
# memory image rather than a bare .COM.  od rather than tr: the input is binary
# and macOS tr rejects it outright with "Illegal byte sequence".
is_zero_padded() {
    [ "$(wc -c < "$1" | tr -d ' ')" -gt 256 ] || return 1
    # No {512} repetition count: BSD grep caps repetition at 255.  "contains no
    # character other than 0" says the same thing and is portable.
    ! head -c 256 "$1" | od -An -v -tx1 | tr -d ' \n' | grep -q '[^0]'
}
ok()    { printf 'ok    %-30s %s\n' "$1" "$2"; }
bad()   { printf 'FAIL  %-30s %s\n' "$1" "$2"; fail=$((fail + 1)); }

# image:diskdef.  hd1k_combo.img has a 1 MB MBR prefix and its slice 0 is
# wbw_hd1k_0; the plain 8 MB hd1k_infocom.img is wbw_hd1k.
IMAGES="disks/hd1k_combo.img:wbw_hd1k_0 disks/hd1k_infocom.img:wbw_hd1k"
UTILS="r8 w8"

for tool in um80 ul80 cpmcp; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "SKIP: $tool is not on PATH - cannot check the disk utilities"
        echo "      (um80/ul80: pip install um80.  cpmcp: cpmtools)"
        exit 0
    fi
done

TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT

# Assemble each utility once, then compare that one build against every image.
for util in $UTILS; do
    src="$ROOT/src/$util.asm"
    if [ ! -f "$src" ]; then
        note "info  $util.asm not present - skipping"
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

    for entry in $IMAGES; do
        img=${entry%%:*}
        def=${entry##*:}
        path="$ROOT/$img"
        [ -f "$path" ] || continue

        # An image legitimately may not carry a given utility, so absence is
        # not a failure - but it is worth saying, because it is also what a
        # wrong diskdef looks like.
        if ! cpmls -f "$def" "$path" 2>/dev/null | grep -qi "^$util\.com$"; then
            note "info  $(basename "$img") holds no $util.com (diskdef $def)"
            continue
        fi

        if ! cpmcp -f "$def" "$path" "0:$util.com" "$TMP/from_img.com" 2>/dev/null; then
            bad "$(basename "$img") $util.com" "could not be extracted (diskdef $def?)"
            continue
        fi

        # ul80 emits a memory image based at 0000h, so its output carries 256
        # leading NOPs and the code sits at file offset 0x100 - CP/M loads the
        # lot at 0100h and slides through the NOPs into the code at 0200h, and
        # every address constant in that build is 256 higher to match.  Some of
        # the binaries in these images were linked that way and some were not.
        #
        # Only the same-layout pair can be compared with cmp.  A build in the
        # other layout is not evidence of drift: the two differ in every address
        # constant by exactly 0x100 whatever the source says, so a byte compare
        # would report a mismatch for a file that is otherwise the same program.
        # Relocating to find out is more than this check is for.
        if is_zero_padded "$TMP/$util.com"; then built_padded=yes; else built_padded=no; fi
        if is_zero_padded "$TMP/from_img.com"; then held_padded=yes; else held_padded=no; fi

        if [ "$built_padded" != "$held_padded" ]; then
            note "info  $(basename "$img") $util.com was linked in the other layout" \
                 "- not comparable"
            note "      (ul80 pads to 0000h; this copy starts at the code.  Every"
            note "       address constant differs by 0x100, so cmp cannot speak here.)"
            continue
        fi

        checked=$((checked + 1))
        if cmp -s "$TMP/$util.com" "$TMP/from_img.com"; then
            ok "$(basename "$img") $util.com" "matches src/$util.asm"
        else
            built=$(wc -c < "$TMP/$util.com" | tr -d ' ')
            held=$(wc -c < "$TMP/from_img.com" | tr -d ' ')
            bad "$(basename "$img") $util.com" \
                "differs from src/$util.asm (built $built bytes, image holds $held)"
            note "      rebuild:  um80 -o $util.rel src/$util.asm && ul80 -o $util.com $util.rel"
            note "                cpmcp -f $def $img $util.com 0:$util.com"
        fi
    done
done

echo
if [ "$checked" -eq 0 ]; then
    echo "SKIP: no disk-resident utilities were found to check"
    exit 0
fi
if [ "$fail" -eq 0 ]; then
    echo "PASS: $checked disk-resident binar$( [ "$checked" -eq 1 ] && echo y || echo ies ) match the source"
    exit 0
fi
echo "FAIL: $fail disk-resident binary/binaries are stale against the source"
exit 1
