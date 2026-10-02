#!/bin/bash
# Builds the release packages into .release/ (outside the repository's
# contents): portable Linux x86-64 and arm64 (in an Ubuntu 22.04 container, with
# podman or docker) and Windows x86-64 (cross-compiled with MinGW-w64).
# The Linux builds are portable .tar.gz archives; .release/PKGBUILD (from
# tools/release/PKGBUILD, with the version and the archives' checksums) lets
# Arch Linux users package the archive they downloaded with makepkg.
# Usage, from the tree's root: tools/release/build.sh [linux-x86_64] [linux-arm64] [windows-x86_64]
# (no arguments: all three). Builds go to build-release-*/; packages, and
# their SHA-256 sums, to .release/.
set -euo pipefail
root=$(pwd)
[ -f CMakeLists.txt ] && [ -d tools/release ] || { echo "run from the leapemu tree's root" >&2; exit 1; }
version=$(sed -n 's/^project(leapemu VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
out=$root/.release
mkdir -p "$out"
targets=("$@")
[ ${#targets[@]} -eq 0 ] && targets=(linux-x86_64 linux-arm64 windows-x86_64)
engine=$(command -v podman || command -v docker || true)
image=leapemu-release-build
docs=(README.md LICENSE THIRD-PARTY.md CHANGELOG.md CONTRIBUTING.md)

# The documents, with the docs/ folder and the README's image, so the README's
# links work in the package.
copy_docs() {  # stage
  cp "${docs[@]}" "$1/"
  cp -r docs "$1/"
  mkdir -p "$1/res" && cp res/leapemu.svg "$1/res/"
}

# Configures, builds and tests one target, then stages its files in $stage.
build() {  # target build-dir stage [cmake args...]
  local target=$1 dir=$2 stage=$3; shift 3
  cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DLEAPEMU_VENDOR_SDL=ON "$@"
  cmake --build "$dir"
  rm -rf "$stage" && mkdir -p "$stage"
}

linux() {  # arch (inside the container)
  local arch=$1 dir=build-release-linux-$1 stage=.release/stage/leapemu-$version-linux-$1
  local args=("-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc")
  [ "$arch" = arm64 ] && args+=(-DCMAKE_TOOLCHAIN_FILE=tools/release/arm64-multiarch.cmake -DLEAPEMU_BUILD_TESTS=OFF)
  build "linux-$arch" "$dir" "$stage" "${args[@]}"
  [ "$arch" = x86_64 ] && ctest --test-dir "$dir" --output-on-failure
  local strip=strip; [ "$arch" = arm64 ] && strip=aarch64-linux-gnu-strip
  cp "$dir/leapemu" "$dir/leapemu-cli" "$stage/" && $strip "$stage/leapemu" "$stage/leapemu-cli"
  cp res/leapemu.desktop res/leapemu.svg "$stage/" && copy_docs "$stage"
  tar -C .release/stage -czf ".release/leapemu-$version-linux-$arch.tar.gz" "leapemu-$version-linux-$arch"
}

if [ "${1:-}" = --in-container ]; then linux "$2"; exit; fi

for t in "${targets[@]}"; do
  case $t in
    linux-x86_64|linux-arm64)
      [ -n "$engine" ] || { echo "podman or docker is needed for $t" >&2; exit 1; }
      "$engine" build -t "$image" tools/release
      "$engine" run --rm -v "$root:/src" -w /src "$image" tools/release/build.sh --in-container "${t#linux-}" ;;
    windows-x86_64)
      dir=build-release-windows-x86_64 stage=.release/stage/leapemu-$version-windows-x86_64
      build "$t" "$dir" "$stage" -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-w64-mingw32.cmake \
        "-DCMAKE_EXE_LINKER_FLAGS=-static -static-libgcc -static-libstdc++"
      if command -v wine > /dev/null; then  # the tests, if Wine can run them
        WINEDEBUG=-all wine "$dir/leapemu-tests.exe" | tail -1
        WINEDEBUG=-all wine "$dir/leapemu-display-tests.exe" | tail -1
      fi
      cp "$dir/leapemu.exe" "$dir/leapemu-cli.exe" "$stage/"
      x86_64-w64-mingw32-strip "$stage/leapemu.exe" "$stage/leapemu-cli.exe"
      copy_docs "$stage"
      # The MinGW-w64 runtime's notices, which its static linking requires.
      notice=
      for f in /usr/share/licenses/mingw-w64-crt/COPYING.MinGW-w64-runtime.txt \
               /usr/share/doc/mingw-w64*/COPYING.MinGW-w64-runtime.txt; do
        [ -f "$f" ] && { notice=$f; break; }
      done
      [ -n "$notice" ] || { echo "the MinGW-w64 runtime's license notices were not found" >&2; exit 1; }
      cp "$notice" "$stage/COPYING.MinGW-w64-runtime.txt"
      (cd .release/stage && rm -f "../leapemu-$version-windows-x86_64.zip" &&
        zip -qr "../leapemu-$version-windows-x86_64.zip" "leapemu-$version-windows-x86_64") ;;
    *) echo "unknown target: $t" >&2; exit 1 ;;
  esac
done
rm -rf .release/stage
# The PKGBUILD for the Linux archives built (so far), with their checksums.
sums() {  # arch tarball
  local sum=SKIP
  [ -f ".release/$2" ] && sum=$(sha256sum ".release/$2" | cut -d' ' -f1)
  echo "sha256sums_$1=('$sum')"
}
sed -e "s/^pkgver=.*/pkgver=$version/" \
    -e "s/^sha256sums_x86_64=.*/$(sums x86_64 "leapemu-$version-linux-x86_64.tar.gz")/" \
    -e "s/^sha256sums_aarch64=.*/$(sums aarch64 "leapemu-$version-linux-arm64.tar.gz")/" \
    tools/release/PKGBUILD > .release/PKGBUILD
(cd .release && sha256sum leapemu-"$version"-*.{tar.gz,zip} PKGBUILD 2> /dev/null > SHA256SUMS || true)
ls -la .release
