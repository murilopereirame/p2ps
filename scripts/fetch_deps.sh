#!/usr/bin/env bash
# Get the build inputs into third_party/:
#   p2f-lib   the PS4 build of p2f-lib (header + libp2f.a), version in
#             P2F_LIB_VERSION, from its GitHub release.
#             P2F_LIB_SRC=/path/to/p2f-lib builds it from a local checkout
#             instead (needs OO_PS4_TOOLCHAIN).
#   font8x8   the 8x8 font (public domain), pinned commit.
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

want=$(tr -d '[:space:]' < P2F_LIB_VERSION)
lib=third_party/p2f-lib

if [ -n "${P2F_LIB_SRC:-}" ]; then
  echo "building p2f-lib from $P2F_LIB_SRC"
  (cd "$P2F_LIB_SRC" && scripts/fetch_deps.sh >/dev/null && make ps4 dist-ps4 PS4_AR="${PS4_AR:-llvm-ar}")
  have=$(tr -d '[:space:]' < "$P2F_LIB_SRC/VERSION")
  rm -rf "$lib" && mkdir -p "$lib"
  tar -xzf "$P2F_LIB_SRC/dist/p2f-lib-$have-ps4.tar.gz" -C "$lib" --strip-components=1
  echo "p2f-lib $have (local build)"
  exit 0
fi

if [ -f "$lib/VERSION" ] && [ "$(tr -d '[:space:]' < "$lib/VERSION")" = "$want" ]; then
  echo "have p2f-lib $want"
  exit 0
fi
url="https://github.com/murilopereirame/p2f-lib/releases/download/v$want/p2f-lib-$want-ps4.tar.gz"
rm -rf "$lib" && mkdir -p "$lib"
curl -fsSL "$url" | tar -xz -C "$lib" --strip-components=1
echo "got p2f-lib $want"
