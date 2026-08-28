#!/bin/sh
#
# Verify that the R8/W8 binaries inside the tracked disk images are what
# src/r8.asm and src/w8.asm currently assemble to.
#
# No build target assembles these .COM files - editing r8.asm or w8.asm does not
# change what any image holds - so the source and the shipped binary can drift
# with nothing to notice.  The last drift, a stale w8.com in hd1k_infocom.img,
# was found by hand and only after the source had already changed underneath it.
# This turns that into a check; disks/rebuild_disk_utils.sh is the fix.
#
# What it does, per image and per utility: assemble the source, link it, extract
# the copy the image holds, and compare.
#
# On its first run this could only check half of what it was asked to: the two
# utilities had been built in different layouts.  w8.com was a ul80 memory image
# - 256 leading NOPs, code at file offset 0x100, every address constant 256
# higher to suit - while r8.com was a bare .COM with the code at offset 0, and
# only a same-layout pair can be compared byte for byte.
#
# That is settled now, and the layout is the bare .COM: `org 0100h` was removed
# from both sources.  M80 assembles each as one relocatable code segment and L80
# bases a .COM at 0100h by itself, so the ORG was applied on top of that base and
# pushed the code to 0200h behind 256 zero bytes.  It ran - CP/M loads the file
# at 0100h and the Z80 slides through the NOPs into the code - which is why it
# went unnoticed, but it carried 256 bytes of file for nothing and it is what
# made the two binaries incomparable.  Both build bare now and both are checked.
#
# disks/rebuild_disk_utils.sh is the other half of this: it assembles and
# installs.  If this script fails, that one is what fixes it.
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
#
# One thing more than "matches the source" is asserted, and only of w8.com: that
# the copy on the image asks HBF_HOST_CAPS before it hands a host path to the
# emulator.  Matching the source is not enough on its own, because an image is
# edited in place - a rebuild that puts back an older w8.com, or an image
# restored from before v1.36, is a W8 that gives a host path to a front end that
# has made no promise about where it lands.  docs/RELEASE_ORDER_2026-08-25.md
# asks for exactly this check.  Nothing in the file's text discriminates: the
# armed 1408-byte build prints the same "Usage: W8 <cpmname> [hostpath]" as the
# 1792-byte interlocked one, so the assertion is on the instruction bytes.

set -u

SELF_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "${1:-$SELF_ROOT}" && pwd)" || exit 1

# Every cpmtools command runs from disks/, which carries its own diskdefs.
# cpmtools reads ./diskdefs if there is one and the system file otherwise, and
# not every distribution's system file has the combo slice definitions - Debian
# and Ubuntu's cpmtools 2.23 has wbw_hd1k but not wbw_hd1k_0, on which this
# check reported hd1k_combo.img as holding neither utility.  See disks/diskdefs.
cpmtool() { ( cd "$ROOT/disks" && "$@" ); }

fail=0
skip=0
checked=0
interlocked=0

note()  { printf '  %s\n' "$*"; }

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
# True when $1 contains `ld b,H_CAPS` / `rst 8` - the three bytes 06 e9 cf that
# w8.asm's check_host_path_safe assembles to.  That sequence is the whole of the
# interlock: W8 asks the emulator whether a guest-supplied host path can escape
# the directory the front end writes to, and refuses the path when the answer is
# "no such call" or "no guarantee".
#
# Bytes, via od, rather than `xxd -p | grep`: xxd is not on a stock Ubuntu runner
# (it ships with vim-common), and a flat hex string also matches across a byte
# boundary - 0xB0 0x6E 0x9C 0xF3 reads as "b06e9cf3" and would answer yes.  The
# spaces od leaves between bytes are what keeps the match aligned, so the
# newlines become spaces rather than being deleted.
has_caps_interlock() {
    od -An -v -tx1 "$1" | tr '\n' ' ' | tr -s ' ' | grep -q ' 06 e9 cf '
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

        # Absence used to be an info line and an exit 0.  It is a failure:
        # every image in the table above is one this repository ships with both
        # utilities on it, and the two ways a copy goes missing are the two
        # this check exists for - someone removed it, or the diskdef is wrong
        # and cpmls is reading a garbage directory.  The stale w8.com in
        # hd1k_infocom.img presented exactly this way, and with it as an info
        # line CI would have gone green over it.
        if ! cpmtool cpmls -f "$def" "$path" 2>/dev/null | grep -qi "^$util\.com$"; then
            bad "$(basename "$img") $util.com" "is not on the image at all"
            note "      Either it was removed - disks/rebuild_disk_utils.sh puts"
            note "      it back - or diskdef $def is wrong for this image, which"
            note "      makes cpmls print a garbage directory rather than fail."
            continue
        fi

        if ! cpmtool cpmcp -f "$def" "$path" "0:$util.com" "$TMP/from_img.com" 2>/dev/null; then
            bad "$(basename "$img") $util.com" "could not be extracted (diskdef $def?)"
            continue
        fi

        # Asserted on the copy the image holds, not on the one just built: what
        # ships is the image, and an armed W8 gets onto one by being restored or
        # copied from an older tree, not by being assembled here.  Asked before
        # the layout and byte comparisons below rather than after, because both
        # of those `continue` on a mismatch and this question is independent of
        # either: a search for three instruction bytes does not care whether the
        # file is a bare .COM or a padded memory image, and "is it stale" and
        # "does it hand over a host path unguarded" are different answers.
        if [ "$util" = w8 ]; then
            if has_caps_interlock "$TMP/from_img.com"; then
                interlocked=$((interlocked + 1))
                ok "$(basename "$img") w8.com" "asks HBF_HOST_CAPS before using a host path"
            else
                bad "$(basename "$img") w8.com" "hands over a host path with no interlock"
                note "      The bytes 06 e9 cf - \`ld b,H_CAPS\` then \`rst 8\` - are not in"
                note "      the copy on this image, so it is a W8 from before v1.36: it"
                note "      passes a guest-supplied host path to whatever emulator opens"
                note "      the image, and that emulator may have made no promise about"
                note "      where the path can reach.  The usage message does not tell"
                note "      the two apart: the armed 1408-byte w8.com in this"
                note "      repository's own history prints the same"
                note "      \"Usage: W8 <cpmname> [hostpath]\" as the 1792-byte"
                note "      interlocked one, which is why the check is on bytes."
                note "      rebuild:  disks/rebuild_disk_utils.sh"
            fi
        fi

        # Both layouts must match before cmp can speak: a bare .COM and a
        # padded memory image of the same program differ in every address
        # constant by exactly 0x100, so a byte compare would call them
        # different whatever the source says.  Since the ORG came out of both
        # sources neither side should ever be padded again, so this is now a
        # tripwire rather than a routine branch - if it fires, an ORG is back.
        if is_zero_padded "$TMP/$util.com"; then built_padded=yes; else built_padded=no; fi
        if is_zero_padded "$TMP/from_img.com"; then held_padded=yes; else held_padded=no; fi

        if [ "$built_padded" != "$held_padded" ]; then
            bad "$(basename "$img") $util.com" "was linked in the other layout"
            note "      One side has 256 leading zero bytes and the other does not,"
            note "      so every address constant differs by 0x100 and cmp cannot"
            note "      speak.  src/$util.asm must have no ORG: L80 bases a .COM at"
            note "      0100h already, and an ORG on top of that puts the code at"
            note "      0200h behind a NOP pad.  built=$built_padded held=$held_padded"
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
            note "      rebuild:  disks/rebuild_disk_utils.sh"
            note "      (by hand, cpmcp will not overwrite - the old copy has to"
            note "       go first: cpmrm -f $def $img 0:$util.com)"
        fi
    done
done

echo
if [ "$checked" -eq 0 ]; then
    # Reaching here means the tools were all present - the loop above exits
    # early otherwise - and every image still yielded nothing to compare. That
    # is a broken check, not a clean run: a wrong diskdef, a renamed image or a
    # deleted source all land here, and each one used to be reported as a pass
    # by `make test`.
    echo "FAIL: nothing was checked, though the tools to check with are present"
    echo "      Expected r8.com and w8.com in: $IMAGES"
    echo "      A wrong diskdef looks exactly like this - cpmls prints a garbage"
    echo "      directory rather than failing, so no name matches."
    exit 1
fi
if [ "$fail" -eq 0 ]; then
    echo "PASS: $checked disk-resident binar$( [ "$checked" -eq 1 ] && echo y || echo ies ) match the source"
    echo "PASS: $interlocked shipped w8.com carr$( [ "$interlocked" -eq 1 ] && echo ies || echo y ) the HBF_HOST_CAPS interlock"
    exit 0
fi
# Not "stale" any more: $fail now also counts a w8.com that is byte-identical to
# the source and still has no interlock, which is a different fault.
echo "FAIL: $fail check(s) failed on the disk-resident binaries"
exit 1
