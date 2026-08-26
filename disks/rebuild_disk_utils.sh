#!/bin/sh
#
# Assemble src/r8.asm and src/w8.asm and put the results into every tracked disk
# image.  This is the other half of disks/verify_disk_utils.sh: that one says the
# images have drifted from the source, this one is what stops it being true.
#
# It exists because the recipe used to live in prose.  Editing r8.asm or w8.asm
# does not change what any image holds - nothing in any build target assembles
# them - so for a long time the way to ship a change was to remember four
# commands and the right diskdef for each image.  A stale w8.com in
# hd1k_infocom.img is what that cost, and it was found by hand months later.
#
# Usage: disks/rebuild_disk_utils.sh [tree_root]
# Exit:  0 both utilities are built and installed in every image
#        1 something failed; no image is left half-written (each utility is
#          removed and re-added one at a time, so a failure mid-run leaves that
#          one image without that one utility - re-run to finish)
#
# Requires um80 + ul80 (pip install um80) and cpmtools.  Unlike the verify
# script this does NOT skip when they are missing: you asked for a rebuild.
#
# The images are tracked in git, so `git diff --stat disks/` after a run says
# exactly which ones changed, and `git checkout disks/` undoes it.

set -u

SELF_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "${1:-$SELF_ROOT}" && pwd)" || exit 1

# cpmtools reads ./diskdefs if there is one and the system file otherwise - one
# or the other - and not every distribution's system file carries the combo
# slice definitions (Debian and Ubuntu's cpmtools 2.23 does not).  disks/
# carries its own, so run every cpmtools command from there.  Image paths below
# are absolute, so the working directory is free to be used for this.
cpmtool() { ( cd "$ROOT/disks" && "$@" ); }

# image:diskdef.  hd1k_combo.img has a 1 MB MBR prefix and its slice 0 is
# wbw_hd1k_0; the plain 8 MB hd1k_infocom.img is wbw_hd1k.  A wrong diskdef does
# not fail loudly here any more than it does in the verify script - cpmls prints
# a garbage directory rather than an error - so the table is explicit.
IMAGES="disks/hd1k_combo.img:wbw_hd1k_0 disks/hd1k_infocom.img:wbw_hd1k"
UTILS="r8 w8"

fail=0
installed=0

for tool in um80 ul80 cpmcp cpmrm cpmls; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: $tool is not on PATH" >&2
        echo "       (um80/ul80: pip install um80.  cpmcp/cpmrm/cpmls: cpmtools)" >&2
        exit 1
    fi
done

TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT

# Assemble once, install everywhere.  No ORG in either source: L80 bases a .COM
# at 0100h by itself and an ORG on top of that leaves 256 leading NOPs - see the
# header of verify_disk_utils.sh.
for util in $UTILS; do
    src="$ROOT/src/$util.asm"
    if [ ! -f "$src" ]; then
        echo "ERROR: $src does not exist" >&2
        exit 1
    fi
    if ! um80 -o "$TMP/$util.rel" "$src" >"$TMP/$util.log" 2>&1; then
        echo "FAIL  src/$util.asm does not assemble:" >&2
        sed 's/^/      /' "$TMP/$util.log" >&2
        exit 1
    fi
    if ! ul80 -o "$TMP/$util.com" "$TMP/$util.rel" >>"$TMP/$util.log" 2>&1; then
        echo "FAIL  src/$util.asm assembles but does not link:" >&2
        sed 's/^/      /' "$TMP/$util.log" >&2
        exit 1
    fi
    printf 'built %-8s %s bytes\n' "$util.com" "$(wc -c < "$TMP/$util.com" | tr -d ' ')"
done

# Does the image's size agree with the format its diskdef implies?  The table
# above is keyed by filename, and a wrong diskdef does not fail - cpmtools reads
# a garbage directory and cpmcp writes at the wrong offset - so replacing an
# image with one of the other format under the same name would have this script
# corrupt a tracked binary while printing "ok ... installed".  A size check is
# not proof of format, but it catches exactly that swap.
#   wbw_hd1k    a plain slice: exactly 8 MB
#   wbw_hd1k_*  a combo: a 1 MB MBR prefix plus whole 8 MB slices
size_matches_def() {
    _size=$(wc -c < "$1" | tr -d ' ')
    case "$2" in
        wbw_hd1k)   [ "$_size" -eq 8388608 ] ;;
        wbw_hd1k_*) [ "$_size" -gt 1048576 ] &&
                    [ $(( (_size - 1048576) % 8388608 )) -eq 0 ] ;;
        *)          return 0 ;;   # unknown def: nothing to check against
    esac
}

for entry in $IMAGES; do
    img=${entry%%:*}
    def=${entry##*:}
    path="$ROOT/$img"
    if [ ! -f "$path" ]; then
        echo "skip  $img (not present)"
        continue
    fi
    if [ ! -w "$path" ]; then
        echo "FAIL  $img is not writable by this user - nothing to do but say so." >&2
        echo "      (cpmrm exits 0 without removing anything on a read-only image," >&2
        echo "       so without this check the run fails later and blames cpmcp.)" >&2
        ls -l "$path" >&2
        fail=$((fail + 1))
        continue
    fi
    if ! size_matches_def "$path" "$def"; then
        echo "FAIL  $img is $(wc -c < "$path" | tr -d ' ') bytes, which is not the" >&2
        echo "      shape diskdef $def describes - refusing to write into it." >&2
        echo "      (Writing through the wrong diskdef lands at the wrong offset" >&2
        echo "       and reports success.)" >&2
        fail=$((fail + 1))
        continue
    fi

    for util in $UTILS; do
        # cpmcp refuses to overwrite, so an existing copy has to go first.  A
        # missing one is not an error: an image may simply not have carried this
        # utility before.
        if cpmtool cpmls -f "$def" "$path" 2>/dev/null | grep -qi "^$util\.com$"; then
            if ! cpmtool cpmrm -f "$def" "$path" "0:$util.com" 2>/dev/null; then
                echo "FAIL  $img: could not remove the old $util.com" >&2
                fail=$((fail + 1))
                continue
            fi
            # cpmrm's exit status is not enough.  On an image the process
            # cannot write it removes nothing and still exits 0, and the next
            # thing to speak is cpmcp with "file already exists" - so the run
            # failed while blaming the copy step, or the diskdef, for a
            # permission problem.  Ask the directory instead of the exit code.
            if cpmtool cpmls -f "$def" "$path" 2>/dev/null | grep -qi "^$util\.com$"; then
                echo "FAIL  $img: $util.com is still on the image after cpmrm" >&2
                echo "      cpmrm exited 0 and removed nothing.  It does that when it" >&2
                echo "      cannot write the image at all:" >&2
                if [ ! -w "$path" ]; then
                    echo "      $img is not writable by this user." >&2
                    ls -l "$path" >&2
                else
                    echo "      the image IS writable, so this is something else -" >&2
                    echo "      a full filesystem, or a diskdef that does not describe" >&2
                    echo "      this image." >&2
                fi
                fail=$((fail + 1))
                continue
            fi
        fi
        if cpmtool cpmcp -f "$def" "$path" "$TMP/$util.com" "0:$util.com" 2>/dev/null; then
            installed=$((installed + 1))
            printf 'ok    %-24s %s installed\n' "$(basename "$img")" "$util.com"
        else
            echo "FAIL  $img: could not install $util.com (diskdef $def?)" >&2
            fail=$((fail + 1))
        fi
    done
done

echo
if [ "$installed" -eq 0 ]; then
    # The final line is a claim about what the images hold. Do not make it
    # having opened none of them - which is what a missing image, or a wrong
    # tree root, used to produce alongside an exit 0.
    if [ "$fail" -ne 0 ]; then
        # Present, and every one of them refused. Saying "none of these is
        # present" here would be a second wrong diagnosis on top of whatever
        # the first one was.
        echo "FAIL: nothing was written - $fail step(s) failed above."
        exit 1
    fi
    echo "FAIL: nothing was written - none of these is present under $ROOT:"
    for entry in $IMAGES; do echo "        ${entry%%:*}"; done
    exit 1
fi
if [ "$fail" -eq 0 ]; then
    echo "PASS: the disk images carry the current r8.com and w8.com"
    echo "      ($installed copies written; check with disks/verify_disk_utils.sh)"
    exit 0
fi
echo "FAIL: $fail install step(s) failed"
exit 1
