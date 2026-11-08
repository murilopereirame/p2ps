#!/usr/bin/env bash
# Get the build inputs:
#   p2f-lib   the git submodule in p2f-lib/ (pinned commit), plus its own
#             third-party sources (mbedTLS, cJSON). The Makefile builds its
#             PS4 library from source.
#   font8x8   the 8x8 font (public domain), pinned commit, in third_party/.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p third_party

fetch() { # dir url ref
  if [ -d "third_party/$1" ]; then echo "have $1"; return; fi
  git clone -q "$2" "third_party/$1"
  git -C "third_party/$1" checkout -q "$3"
  echo "got $1 @ $3"
}

fetch font8x8 https://github.com/dhepper/font8x8.git 8e279d2d864e79128e96188a6b9526cfa3fbfef9

if [ ! -f p2f-lib/Makefile ]; then
  git submodule update --init p2f-lib
fi
p2f-lib/scripts/fetch_deps.sh
echo "p2f-lib $(tr -d '[:space:]' < p2f-lib/VERSION) @ $(git -C p2f-lib rev-parse --short HEAD)"
