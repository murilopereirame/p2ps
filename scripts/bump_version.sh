#!/usr/bin/env bash
# Bump the app version (VERSION in the Makefile, "XX.YY" as the PS4 shows it).
#   scripts/bump_version.sh minor     1.00 -> 1.01
#   scripts/bump_version.sh major     1.07 -> 2.00
#   scripts/bump_version.sh 1.20      set it
# Prints the new version.
set -euo pipefail
cd "$(dirname "$0")/.."

cur=$(sed -n 's/^VERSION *:= *\([0-9][0-9]*\.[0-9][0-9]\) *$/\1/p' Makefile)
[ -n "$cur" ] || { echo "no VERSION := XX.YY line in the Makefile" >&2; exit 1; }
major=$((10#${cur%.*}))
minor=$((10#${cur#*.}))

case "${1:-}" in
  minor)
    minor=$((minor + 1))
    if [ "$minor" -gt 99 ]; then major=$((major + 1)); minor=0; fi
    ;;
  major)
    major=$((major + 1)); minor=0
    ;;
  [0-9]*.[0-9][0-9])
    major=$((10#${1%.*})); minor=$((10#${1#*.}))
    ;;
  *)
    echo "usage: $0 minor|major|XX.YY" >&2; exit 2
    ;;
esac
[ "$major" -le 99 ] || { echo "version too big" >&2; exit 1; }

new=$(printf '%d.%02d' "$major" "$minor")
if [ "$new" = "$cur" ]; then echo "already $cur" >&2; exit 1; fi
sed -i.bak "s/^VERSION *:= *$cur *\$/VERSION     := $new/" Makefile && rm -f Makefile.bak
echo "$new"
