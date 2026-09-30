#!/usr/bin/env bash
# Packages the KUDA-Lite source tree for installing on nodes by hand (without git or the SSH
# deploy script): dist/kudalite-<version>.tar.gz plus a .sha256 checksum file.
#
#   scripts/package-release.sh [OUTPUT_DIR]     (default: dist/)
#
# On a node:  tar xzf kudalite-<version>.tar.gz && cd kudalite-<version>
#             sudo deploy/install.sh worker --controller pi5-ctl
# Works on Linux and macOS.
set -euo pipefail

src="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-$src/dist}"
version="$(sed -n 's/^  VERSION \([0-9.]*\)$/\1/p' "$src/CMakeLists.txt" | head -1)"
[[ -n "$version" ]] || { echo "cannot read the version from CMakeLists.txt" >&2; exit 1; }
name="kudalite-$version"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
rsync -a \
  --exclude .git --exclude /build/ --exclude /dist/ --exclude node_modules --exclude .next \
  --exclude .venv --exclude __pycache__ --exclude .pytest_cache --exclude '*.tsbuildinfo' \
  --exclude deploy/cluster.inventory --exclude deploy/rendered \
  "$src/" "$stage/$name/"

mkdir -p "$out"
tar -C "$stage" -czf "$out/$name.tar.gz" "$name"
(cd "$out" && if command -v sha256sum >/dev/null; then sha256sum "$name.tar.gz"; else shasum -a 256 "$name.tar.gz"; fi >"$name.tar.gz.sha256")

echo "$out/$name.tar.gz ($(du -h "$out/$name.tar.gz" | cut -f1), $(tar -tzf "$out/$name.tar.gz" | grep -vc '/$') files)"
echo "sha256: $(cut -d' ' -f1 "$out/$name.tar.gz.sha256")"
