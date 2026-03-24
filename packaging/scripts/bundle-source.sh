#!/usr/bin/env sh
# bundle-source.sh - Bundle the VoiceOS source tree into a tarball that
# abuild can consume.  Run this from anywhere in the repo before building APKs.
#
# Output: packaging/aports/baltek-vos-ui/baltek-vos-ui-<version>.tar.gz
#
# The tarball extracts to:
#   baltek-vos-ui-<version>/
#     src/           ← VoiceOS Python source (ui/vos/src/)
#     requirements.txt
#
# After running this, regenerate checksums on Alpine with:
#   cd packaging/aports/baltek-vos-ui && abuild checksum

set -eu

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APKBUILD="$REPO_ROOT/packaging/aports/baltek-vos-ui/APKBUILD"

# Read version from APKBUILD so they always stay in sync
PKGVER=$(grep '^pkgver=' "$APKBUILD" | cut -d= -f2)
TARNAME="baltek-vos-ui-${PKGVER}"
OUTDIR="$REPO_ROOT/packaging/aports/baltek-vos-ui"
TARBALL="$OUTDIR/${TARNAME}.tar.gz"

VOS_SRC="$REPO_ROOT/ui/vos"

if [ ! -d "$VOS_SRC/src" ]; then
  echo "ERROR: VoiceOS source not found at $VOS_SRC/src" >&2
  exit 1
fi

echo "Bundling VoiceOS ${PKGVER} source..."

# Build the tarball from a clean temp staging dir so the internal layout
# is exactly what APKBUILD expects: baltek-vos-ui-<ver>/{src/,requirements.txt}
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

STAGEPKG="$STAGE/$TARNAME"
mkdir -p "$STAGEPKG"

cp -r "$VOS_SRC/src"             "$STAGEPKG/src"
cp    "$VOS_SRC/requirements.txt" "$STAGEPKG/requirements.txt"

tar -C "$STAGE" -czf "$TARBALL" "$TARNAME"

echo "Created: $TARBALL"
echo ""
echo "Next steps (run on Alpine with alpine-sdk installed):"
echo "  cd packaging/aports/baltek-vos-ui && abuild checksum"
echo "  cd packaging/aports/baltek-dte-config && abuild checksum"
