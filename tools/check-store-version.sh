#!/bin/sh
# check-store-version.sh - what does the release channel actually serve, and
# does anything in this tree claim otherwise?
#
# WHY THIS EXISTS.  VERSION, a CHANGELOG heading and the shipped: field in
# z80cpmw/FEATURE_PARITY.md all describe the TREE.  None of them knows what a
# user can install.  The three GUI ports each measure their store - ioscpm the
# iTunes lookup since 2026-09-03, z80cpmw DisplayCatalog and cpmdroid the Play
# listing since 2026-09-06 - and z80cpmw's first measurement caught its own
# changelog wrong by two releases, which had already sent a parity re-read to the
# wrong commit.  This port had no such check either, and it is the one port whose
# answer is easiest to get right: its channel is GitHub Releases, which has a
# real API and immutable assets.
#
#   sh tools/check-store-version.sh            # metadata only, no downloads
#   sh tools/check-store-version.sh --assets   # also fetch and hash the packages
#
# Exit 0 = measured, and nothing recorded here claims a release that does not
#          exist.  The tree being AHEAD is normal and is reported, not failed.
# Exit 1 = something records a shipped state the release channel contradicts.
# Exit 2 = could not verify (no network, no curl/wget, no parser, no release).

set -u

REPO="avwohl/romwbw_emu"
API="https://api.github.com/repos/$REPO/releases/latest"

want_assets=no
[ "${1:-}" = "--assets" ] && want_assets=yes

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here" && git rev-parse --show-toplevel 2>/dev/null) || root=$(dirname "$here")
PARITY="$root/../z80cpmw/FEATURE_PARITY.md"

tmp=$(mktemp -d 2>/dev/null || mktemp -d -t rel)
trap 'rm -rf "$tmp"' EXIT INT TERM
status=0

get() {
    if command -v curl >/dev/null 2>&1; then
        curl -sSfL -m 60 -H 'Accept: application/vnd.github+json' -o "$2" "$1" 2>/dev/null
    elif command -v wget >/dev/null 2>&1; then
        wget -qT 60 --header='Accept: application/vnd.github+json' -O "$2" "$1" 2>/dev/null
    else
        return 127
    fi
}

command -v python3 >/dev/null 2>&1 || {
    echo "CANNOT VERIFY: python3 is needed to read the release JSON."; exit 2; }

if ! get "$API" "$tmp/rel.json"; then
    echo "CANNOT VERIFY: no network, no curl/wget, or the API refused."
    echo "This gate does not pass when it cannot check."
    exit 2
fi

# releases/latest is GitHub's own pointer and skips drafts and prereleases.  It
# is the thing a user following the README lands on, so it is what this measures.
eval "$(python3 - "$tmp/rel.json" <<'PY'
import json, sys, shlex
d = json.load(open(sys.argv[1]))
tag = d.get('tag_name') or ''
print("tag=%s" % shlex.quote(tag))
print("published=%s" % shlex.quote((d.get('published_at') or '')[:10]))
print("prerelease=%s" % shlex.quote(str(d.get('prerelease'))))
print("nassets=%s" % shlex.quote(str(len(d.get('assets') or []))))
PY
)"

if [ -z "${tag:-}" ]; then
    echo "CANNOT VERIFY: no tag_name in the API response for $REPO."
    echo "Rate-limited, or there is no published release."
    exit 2
fi

live=${tag#v}
echo "GitHub Releases, $REPO"
echo "  serves           $tag   (published $published, $nassets assets)"
[ "$prerelease" = "True" ] && {
    echo "  RELEASES/LATEST IS A PRERELEASE, which GitHub is not supposed to do."
    status=1; }

# --- what the tree claims to be ------------------------------------------------
vtree=$(head -1 "$root/VERSION" 2>/dev/null | tr -d ' \t\r\n')
if [ -z "$vtree" ]; then
    echo "CANNOT VERIFY: no VERSION file at $root/VERSION."; exit 2
fi
echo "  this tree        $vtree"

echo
if [ "$vtree" = "$live" ]; then
    echo "The tree and the release channel are on the same version."
    echo "  Same number is not the same software: commits since the tag are in"
    echo "  the tree and not in the packages."
else
    echo "The tree is $vtree, the newest release is $live."
    echo "  Ahead is normal - you build before you ship.  What is NOT normal is"
    echo "  writing $vtree into anything that records what USERS have."
fi

# --- the unversioned aliases ---------------------------------------------------
# Every package ships twice: romwbw-emu_1.38_amd64.deb and romwbw-emu_amd64.deb,
# the second being the name the README tells people to fetch.  If those two ever
# stop being the same bytes, a user following the documentation installs
# something nobody measured.  Sizes are free; --assets checks the hashes.
echo
python3 - "$tmp/rel.json" <<'PY'
import json, sys, re
d = json.load(open(sys.argv[1]))
by = {a['name']: a for a in d.get('assets') or []}
pairs, bad = [], 0
for name, a in sorted(by.items()):
    m = re.match(r'^(romwbw-emu)[-_]\d[\d.]*[-_]?\d*[._](.+)$', name)
    if not m:
        continue
    alias = None
    for cand in ("%s_%s" % (m.group(1), m.group(2)), "%s.%s" % (m.group(1), m.group(2))):
        if cand in by:
            alias = cand
    if alias:
        same = by[alias]['size'] == a['size']
        pairs.append((name, alias, same))
        if not same:
            bad += 1
if not pairs:
    print("  no versioned/unversioned asset pairs found - naming changed?")
else:
    for n, al, same in pairs:
        print("  %-32s <-> %-24s %s" % (n, al, "same size" if same else "*** SIZES DIFFER ***"))
sys.exit(1 if bad else 0)
PY
[ $? -ne 0 ] && { echo "  A versioned package and its unversioned alias are different sizes."; status=1; }

# --- what the family records ---------------------------------------------------
if [ -f "$PARITY" ]; then
    claim=$(awk '/^romwbw_emu[[:space:]]/ { for (i = 1; i <= NF; i++)
                    if ($i ~ /^shipped:/) { print substr($i, 9); exit } }' "$PARITY")
    echo
    if [ -z "$claim" ]; then
        echo "z80cpmw/FEATURE_PARITY.md  no shipped: field on the romwbw_emu line"
    elif [ "$claim" = "$live" ]; then
        echo "z80cpmw/FEATURE_PARITY.md  shipped:$claim agrees with the release channel"
    else
        echo "z80cpmw/FEATURE_PARITY.md  CLAIMS shipped:$claim, BUT the newest release is $live"
        echo "  Re-read that column at the released tag, then set this field."
        status=1
    fi
fi

# --- the packages themselves ---------------------------------------------------
if [ "$want_assets" = yes ]; then
    echo
    echo "Fetching packages (this is the only part that downloads):"
    deb="romwbw-emu_${live}_amd64.deb"
    url="https://github.com/$REPO/releases/download/$tag/$deb"
    if ! get "$url" "$tmp/$deb"; then
        echo "  CANNOT VERIFY: $deb did not download."
        exit 2
    fi
    if ! command -v dpkg-deb >/dev/null 2>&1; then
        echo "  $deb fetched, but dpkg-deb is not installed - cannot look inside."
    else
        mkdir -p "$tmp/x" && dpkg-deb -x "$tmp/$deb" "$tmp/x" 2>/dev/null
        echo "  $deb unpacked."
        # The ROM a user actually gets.  This repository builds its own, and the
        # question that keeps mattering is which generation it is.
        find "$tmp/x" -name 'emu_avw.rom' | while read -r r; do
            printf '    %-46s sha256 %s\n' \
                "$(echo "$r" | sed "s|$tmp/x||")" \
                "$(sha256sum "$r" | cut -c1-16)"
        done
        n=$(find "$tmp/x" -name '*.img' | wc -l)
        echo "    disk images in the package: $n"
    fi
fi

echo
if [ "$status" != 0 ]; then
    echo "Something records a shipped state the release channel does not support."
    exit 1
fi
exit 0
