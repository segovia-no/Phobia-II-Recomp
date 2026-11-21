#!/usr/bin/env bash
# One command per release kit: fetch the pinned tools, build the host program, package.
#   scripts/release/build_kit.sh macos-arm64|macos-x86_64|linux-x86_64|linux-arm64 [version]
# macOS kits build on a Mac (Xcode CLT + cmake); Linux kits build in Docker from any host.
# Output: build/release/PhobiaIIRecomp-<version>-<platform>.tar.gz (no game code inside).
set -euo pipefail
PLAT="${1:?platform: macos-arm64|macos-x86_64|linux-x86_64|linux-arm64}"
VERSION="${2:-$(git -C "$(dirname "$0")" describe --always --dirty 2>/dev/null || echo dev)}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OS="${PLAT%%-*}"; ARCH="${PLAT##*-}"
PY="$ROOT/.venv/bin/python"; [ -x "$PY" ] || PY=python3
"$PY" -c 'import sys; sys.exit(sys.version_info < (3, 12))' || { echo "needs Python 3.12+ ($PY)"; exit 1; }
TOOLS="$ROOT/build/tools/$PLAT"
[ -x "$TOOLS/zig/zig" ] || "$PY" "$ROOT/scripts/release/fetch_tools.py" --platform "$PLAT" --out "$TOOLS"
case "$OS" in
  macos) [ "$(uname -s)" = Darwin ] || { echo "macOS kits build on a Mac"; exit 1; }
         "$ROOT/scripts/release/build_macos_host.sh" "$ARCH" ;;
  linux) "$ROOT/scripts/release/build_linux_host.sh" "$ARCH" ;;
  *) echo "unknown platform $PLAT"; exit 1 ;;
esac
"$PY" "$ROOT/scripts/release/package.py" --platform "$PLAT" --host "$ROOT/build-$OS-$ARCH/phobia2" \
      --tools "$TOOLS" --version "$VERSION"
