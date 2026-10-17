#!/usr/bin/env bash
# Build the Arch .pkg.tar.zst for GETH from a clean `git archive` of the
# current tree.
#
#   1. checks: clean tree, CMake VERSION == PKGBUILD pkgver, CMake
#      GETH_LICENSE == PKGBUILD license=, LICENSE file present;
#   2. `git archive` of HEAD into geth-<pkgver>.tar.gz;
#   3. stages the PKGBUILD + geth.install + the geth tarball + the pinned
#      dependency tarballs (fetched once, cached under
#      ${XDG_CACHE_HOME:-$HOME/.cache}/geth-package, sha256-verified) into a
#      temp dir;
#   4. `makepkg -f --cleanbuild` as the current (normal) user — build() does
#      no network access (dependencies come from FETCHCONTENT_SOURCE_DIR_*);
#   5. copies the package to dist/ and writes dist/SHA256SUMS.
#
# Needs base-devel (makepkg, a C++ compiler) and cmake. No root.
#
# Usage: tools/package-arch.sh
#   (or: cmake --build build-rel --target package-arch)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

die() { echo "package-arch: $*" >&2; exit 1; }

command -v makepkg >/dev/null || die "makepkg not found (install base-devel)"
command -v cmake >/dev/null || die "cmake not found"

# ---------------------------------------------------------------- versions --
CMAKE_VER="$(grep -A4 '^project(geth' CMakeLists.txt \
  | grep -m1 -oE 'VERSION [0-9]+(\.[0-9]+)*' | awk '{print $2}')"
CMAKE_LIC="$(sed -n 's/^set(GETH_LICENSE[[:space:]]*"\([^"]*\)".*/\1/p' CMakeLists.txt | head -n1)"
PKG="packaging/arch/PKGBUILD"
[[ -f "$PKG" ]] || die "missing $PKG"
PKG_VER="$(grep -m1 '^pkgver=' "$PKG" | cut -d= -f2)"
PKG_LIC="$(grep -m1 '^license=(' "$PKG" | sed 's/^license=(//; s/)//; s/[[:space:]]//g')"

[[ -n "$CMAKE_VER" ]] || die "cannot read project VERSION from CMakeLists.txt"
[[ -n "$CMAKE_LIC" ]] || die "cannot read GETH_LICENSE from CMakeLists.txt"
[[ -n "$PKG_VER" ]] || die "cannot read pkgver from $PKG"

[[ "$CMAKE_VER" == "$PKG_VER" ]] || \
  die "version mismatch: CMakeLists.txt is $CMAKE_VER, PKGBUILD pkgver is $PKG_VER"
[[ "$CMAKE_LIC" == "$PKG_LIC" ]] || \
  die "license mismatch: CMakeLists.txt is '$CMAKE_LIC', PKGBUILD license= is '$PKG_LIC'"
[[ -f LICENSE ]] || die "LICENSE file missing (the install rule needs it)"

echo "version:  $CMAKE_VER"
echo "license:  $CMAKE_LIC"

# ----------------------------------------------------------------- staging --
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SRC="$WORK/src"
mkdir -p "$SRC"

# A release tarball must describe a commit, not a dirty working tree.
if [[ -n "$(git status --porcelain)" ]]; then
  die "working tree is dirty — commit (or stash) first, then re-run"
fi
echo "commit:   $(git rev-parse --short HEAD)"

git archive --format=tar --prefix="geth-${CMAKE_VER}/" HEAD \
  | gzip -9 > "$SRC/geth-${CMAKE_VER}.tar.gz"

# Pinned dependency tarballs: fetched once into the cache, sha256-verified
# every time, then staged. build() itself never touches the network.
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/geth-package"
mkdir -p "$CACHE"
fetch_pin() { # name url sha256
  local name="$1" url="$2" sha="$3"
  if [[ -f "$CACHE/$name" ]]; then
    echo "cached:   $name"
  else
    echo "fetching: $name"
    curl -fL --retry 3 -o "$CACHE/$name.part" "$url"
    if ! echo "$sha  $CACHE/$name.part" | sha256sum --check - >/dev/null; then
      rm -f "$CACHE/$name.part"
      die "sha256 mismatch for $name (expected $sha)"
    fi
    mv "$CACHE/$name.part" "$CACHE/$name"
  fi
  echo "$sha  $CACHE/$name" | sha256sum --check - >/dev/null \
    || die "cached $name failed sha256 verification"
  cp "$CACHE/$name" "$SRC/$name"
}
fetch_pin ftxui-6.1.9.tar.gz \
  https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v6.1.9.tar.gz \
  45819c1e54914783d4a1ca5633885035d74146778a1f74e1213cdb7b76340e71
fetch_pin tomlplusplus-3.4.0.tar.gz \
  https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz \
  8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
fetch_pin doctest-2.4.11.tar.gz \
  https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz \
  632ed2c05a7f53fa961381497bf8069093f0d6628c5f26286161fbd32a560186

cp "$PKG" "$SRC/PKGBUILD"
cp packaging/arch/geth.install "$SRC/geth.install"

# ------------------------------------------------------------------ build ---
echo "building package (makepkg -f --cleanbuild) ..."
# makepkg (pacman 7.x) takes no positional SRC_DIR — pass it with --dir.
makepkg -f --cleanbuild --nocolor --dir "$SRC"

PKG_FILE="$(ls "$SRC"/geth-${CMAKE_VER}-*-x86_64.pkg.tar.zst)"
[[ -f "$PKG_FILE" ]] || die "makepkg did not produce a package in $SRC"

# ------------------------------------------------------------------- dist ---
mkdir -p dist
DEST="dist/$(basename "$PKG_FILE")"
cp "$PKG_FILE" "$DEST"
(cd dist && sha256sum geth-${CMAKE_VER}-*-x86_64.pkg.tar.zst > SHA256SUMS)

echo
echo "done. Package saved to:"
echo "  $REPO_ROOT/$DEST"
echo "  checksum: $REPO_ROOT/dist/SHA256SUMS"
echo
ls -la dist/
echo
cat dist/SHA256SUMS
