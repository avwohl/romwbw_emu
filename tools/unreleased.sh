#!/bin/sh
# unreleased.sh - what is finished here but not yet in anybody's hands?
#
# WHY THIS EXISTS.  "Done" means four different things in this family and the
# difference is where the mistakes come from: written, built, published, and
# installed.  CHANGELOG.md describes the TREE.  A tag says somebody cut a
# release.  A GitHub release with assets on it is the first point a Linux user
# can get anything.  And the disk images are a different repository on a
# different schedule, so a fix in an image reaches users through romwbw_disks
# publishing, with no release of this repository at all.
#
# This script answers the one question those four confuse: what have I got that
# they have not?  It reports.  It does not judge.
#
#   sh tools/unreleased.sh              code and disks
#   sh tools/unreleased.sh --code       skip the disks half
#   sh tools/unreleased.sh --disks      skip the code half
#
# HOW THIS RUNS: BY HAND, AND IT MUST STAY THAT WAY.  This is deliberately not
# wired to any workflow, and the exit codes below are shaped so that it cannot
# usefully become one.
#
# Exit 0 = it measured, INCLUDING when the answer is "eleven commits and two
#          fixes are unreleased".  That is the normal state of a working
#          repository - you commit before you tag and tag before you publish -
#          and a check that goes red for it is red every day.  This repository
#          had four such jobs until 2026-09-13 and they were all deleted for
#          exactly that reason; do not rebuild one here.
# Exit 2 = could not measure: no network, no gh, no curl, no sibling checkout.
#          Nothing is asserted when nothing was read.
#
# There is no exit 1.  If you want one, you want a different script.

set -u

want_code=yes
want_disks=yes
case "${1:-}" in
    --code)  want_disks=no ;;
    --disks) want_code=no ;;
    "")      ;;
    *)       echo "usage: sh tools/unreleased.sh [--code|--disks]" >&2; exit 2 ;;
esac

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here" && git rev-parse --show-toplevel 2>/dev/null) || {
    echo "CANNOT MEASURE: $here is not inside a git checkout."
    exit 2
}
SIBLINGS=$(dirname "$root")
DISKS="$SIBLINGS/romwbw_disks"

measured=no

# ---------------------------------------------------------------------------
# The code half.
# ---------------------------------------------------------------------------
if [ "$want_code" = yes ]; then
    echo "CODE - romwbw_emu"
    echo

    tree_version=$(head -1 "$root/VERSION" 2>/dev/null | tr -d ' \t\r')
    if [ -z "$tree_version" ]; then
        echo "  CANNOT MEASURE: no VERSION file at $root/VERSION."
        exit 2
    fi

    # What a user can install.  The GitHub release is the channel for this port -
    # there is no store - so "published with assets on it" is the whole test.
    if ! command -v gh >/dev/null 2>&1; then
        echo "  CANNOT MEASURE: gh is not installed, so the published release"
        echo "  cannot be read.  The tree is $tree_version."
        exit 2
    fi

    published=$(gh release list --repo avwohl/romwbw_emu --limit 1 \
                    --json tagName,isLatest --jq '.[] | select(.isLatest) | .tagName' \
                2>/dev/null)
    if [ -z "$published" ]; then
        echo "  CANNOT MEASURE: no release marked Latest, or the API is unreachable."
        exit 2
    fi
    measured=yes

    asset_count=$(gh release view "$published" --repo avwohl/romwbw_emu \
                      --json assets --jq '.assets | length' 2>/dev/null)
    echo "  tree VERSION      $tree_version"
    echo "  newest release    $published  (${asset_count:-0} asset(s))"
    if [ "${asset_count:-0}" = "0" ]; then
        echo "                    ^ a release with no assets installs nothing."
    fi
    echo

    # The tag the release names.  A release can exist without the tag being in
    # this checkout, which is itself worth saying rather than silently skipping.
    if ! git -C "$root" rev-parse --verify --quiet "$published^{commit}" >/dev/null 2>&1; then
        echo "  $published is not a tag in this checkout - fetch, then re-run."
        echo "  Everything below would be measured from the wrong place."
        exit 2
    fi

    n=$(git -C "$root" rev-list --count "$published..HEAD" 2>/dev/null)
    if [ "${n:-0}" = "0" ]; then
        echo "  Nothing since $published.  The tree is the release."
    else
        echo "  $n commit(s) in the tree and not in $published:"
        echo
        git -C "$root" log --format='    %h  %ad  %s' --date=short "$published..HEAD"
        echo

        # src/ is the part that matters to the three GUI ports, and it does NOT
        # travel by release: ioscpm symlinks into src/, z80cpmw's vcxproj
        # compiles it in place, cpmdroid's CMakeLists reads a sibling checkout.
        # So a src/ commit is already downstream's problem, released or not,
        # while a web/ or packaging commit reaches nobody until a release.
        src_n=$(git -C "$root" rev-list --count "$published..HEAD" -- src/ 2>/dev/null)
        echo "  Of those, $src_n touch src/."
        if [ "${src_n:-0}" != "0" ]; then
            echo "    src/ does not travel by release.  ioscpm symlinks into it,"
            echo "    z80cpmw compiles it in place and cpmdroid reads a sibling"
            echo "    checkout, so these reach all three on their next build,"
            echo "    tag or no tag.  Check DOWNSTREAM.md for whether any of"
            echo "    them changes the build contract."
            git -C "$root" log --format='      %h  %s' "$published..HEAD" -- src/
        fi
        echo
        echo "  Everything else here reaches a Linux user only when you publish"
        echo "  a release: the .deb and .rpm are built by release.yml on"
        echo "  'release: published', so a tag alone ships nothing."
    fi
    echo

    # CHANGELOG entries above the released version are written-up work that has
    # not gone out.  Entries BELOW it are history and are not interesting here.
    newest_entry=$(grep -oE '^## \[[0-9][0-9.]*\]' "$root/CHANGELOG.md" 2>/dev/null |
                   head -1 | tr -d '#[] ')
    if [ -n "${newest_entry:-}" ]; then
        rel=${published#v}
        if [ "$newest_entry" != "$rel" ]; then
            echo "  CHANGELOG.md's newest entry is [$newest_entry] and the release is $rel."
            echo "  Read that entry: it describes work users do not have."
        elif [ "${n:-0}" = "0" ]; then
            echo "  CHANGELOG.md's newest entry is [$newest_entry], which IS the"
            echo "  released version.  Nothing is owed."
        else
            echo "  CHANGELOG.md's newest entry is [$newest_entry], which IS the"
            echo "  released version - so the commits above are unwritten as well"
            echo "  as unreleased."
        fi
    fi
    echo
fi

# ---------------------------------------------------------------------------
# The disks half.  A different repository, a different channel, and the one
# that reaches installed clients with no release of anything else.
# ---------------------------------------------------------------------------
if [ "$want_disks" = yes ]; then
    echo "DISKS - romwbw_disks"
    echo

    if [ ! -d "$DISKS/.git" ]; then
        echo "  NOT CHECKED OUT at $DISKS - the disks half cannot be answered here."
        echo "  Clone it beside this repository and re-run."
        [ "$measured" = yes ] && exit 0
        exit 2
    fi

    get() { # $1 url, $2 dest
        if command -v curl >/dev/null 2>&1; then
            curl -sSfL -m 45 -o "$2" "$1" 2>/dev/null
        elif command -v wget >/dev/null 2>&1; then
            wget -qT 45 -O "$2" "$1" 2>/dev/null
        else
            return 127
        fi
    }

    tmp=$(mktemp -d 2>/dev/null || mktemp -d -t unreleased)
    trap 'rm -rf "$tmp"' EXIT INT TERM

    # The address every client compiles in.  If this and the committed catalog
    # differ, the difference is sitting in romwbw_disks unpublished - and that
    # is the one gap in this whole family that reaches INSTALLED clients the
    # moment it closes, with no app release on any platform.
    INDEX_URL="https://github.com/avwohl/romwbw_disks/releases/download/catalog-v0/index-v0.json"
    if ! get "$INDEX_URL" "$tmp/pub.json"; then
        echo "  CANNOT MEASURE: could not fetch the published index."
        echo "  $INDEX_URL"
        [ "$measured" = yes ] && exit 0
        exit 2
    fi

    committed="$DISKS/catalog/v0/index.json"
    if [ ! -f "$committed" ]; then
        echo "  CANNOT MEASURE: no committed catalog at $committed."
        [ "$measured" = yes ] && exit 0
        exit 2
    fi

    echo "  published index   catalog-v0 release asset"
    echo "  committed index   catalog/v0/index.json"
    if cmp -s "$tmp/pub.json" "$committed"; then
        echo "  They are byte-identical.  Every client fetching the catalog is"
        echo "  getting what this tree says it should."
    else
        echo
        echo "  THEY DIFFER.  What is committed has not been published, so no"
        echo "  installed client is seeing it.  Publishing is an asset upload to"
        echo "  the catalog-v0 release - no build of any port is involved."
        echo
        if command -v diff >/dev/null 2>&1; then
            diff "$tmp/pub.json" "$committed" 2>/dev/null |
                sed 's/^/    /' | head -40
        fi
    fi
    echo

    # Per-release catalogs, the layer down: an image can be re-cut and committed
    # without the index changing at all.  The addresses come OUT OF THE PUBLISHED
    # INDEX rather than being built here - that two-hop resolution is the whole
    # design, and a script that guessed the URL would be testing its own guess.
    sed 's/[{,]/\n/g' "$tmp/pub.json" |
        sed -n 's/.*"catalog_url"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' |
        while read -r url; do
            [ -n "$url" ] || continue
            fname=${url##*/}
            relname=${fname#catalog-v0-}
            relname=${relname%.json}
            cat="$DISKS/catalog/v0/$relname/catalog.json"
            if [ ! -f "$cat" ]; then
                echo "  RomWBW $relname  published, but no committed catalog at"
                echo "                  catalog/v0/$relname/catalog.json"
                continue
            fi
            if ! get "$url" "$tmp/cat.json"; then
                echo "  RomWBW $relname  could not fetch $url"
                continue
            fi
            if cmp -s "$tmp/cat.json" "$cat"; then
                echo "  RomWBW $relname  catalog matches what is published"
            else
                echo "  RomWBW $relname  COMMITTED CATALOG DIFFERS FROM PUBLISHED -"
                echo "                  an image or a hash here is not what clients fetch"
            fi
        done
    echo

    # Commits in that tree since the catalog was last published.  Not all of
    # them touch a catalog, so this is an upper bound and says so.
    pub_at=$(gh release view catalog-v0 --repo avwohl/romwbw_disks \
                 --json publishedAt --jq .publishedAt 2>/dev/null)
    if [ -n "${pub_at:-}" ]; then
        # Only what could possibly change what a client fetches.  The unfiltered
        # list is thirty-odd commits of tooling and prose and reads as alarming
        # while meaning nothing, which is how a report gets ignored.
        since=$(git -C "$DISKS" log --since="$pub_at" --format='    %h  %ad  %s' \
                    --date=short -- catalog/ help/ 2>/dev/null)
        other=$(git -C "$DISKS" rev-list --count --since="$pub_at" HEAD 2>/dev/null)
        touching=$(git -C "$DISKS" rev-list --count --since="$pub_at" HEAD -- catalog/ help/ 2>/dev/null)
        echo "  romwbw_disks since the catalog was published ($pub_at):"
        echo "  ${touching:-0} commit(s) touching catalog/ or help/, of ${other:-0} in total."
        if [ -n "$since" ]; then
            echo
            echo "$since"
            echo
            echo "  Those are the ones that could change what a client fetches."
            echo "  If the comparisons above say everything matches, they have"
            echo "  already been published."
        fi
    fi
    echo
fi

exit 0
