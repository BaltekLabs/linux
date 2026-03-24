#!/usr/bin/env bash
# build_apk_repo.sh - Build signed APK packages and produce a local repo index.
#
# Prerequisites (run inside Alpine with these packages installed):
#   apk add alpine-sdk abuild openssl
#
# Usage:
#   ./packaging/scripts/build_apk_repo.sh
#
# Output: packaging/out/apkrepo/  (serve over HTTP to use as an APK repo)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPTS_DIR="$ROOT/scripts"
APORTS_DIR="$ROOT/aports"
OUT_DIR="$ROOT/out/apkrepo"
KEY_DIR="$ROOT/keys"
REPO_NAME="baltek"

mkdir -p "$OUT_DIR" "$KEY_DIR"

# ── Step 1: Bundle the VoiceOS Python source into a tarball ─────────────────
echo "[1/7] Bundling VoiceOS source..."
"$SCRIPTS_DIR/bundle-source.sh"

# ── Step 2: Ensure abuild toolchain is present ───────────────────────────────
echo "[2/7] Checking for alpine-sdk / abuild..."
if ! command -v abuild >/dev/null 2>&1; then
  echo "  abuild not found. Install with: apk add alpine-sdk abuild openssl"
  exit 1
fi

# ── Step 3: Signing key ───────────────────────────────────────────────────────
echo "[3/7] Ensuring abuild signing key..."
if ! ls "$KEY_DIR"/*.rsa >/dev/null 2>&1; then
  echo "  Generating key (stored in $KEY_DIR)..."
  abuild-keygen -a -n
  mkdir -p "$HOME/.abuild"
  cp -a "$HOME/.abuild/"*.rsa "$KEY_DIR/" 2>/dev/null || true
  cp -a "$HOME/.abuild/"*.pub "$KEY_DIR/" 2>/dev/null || true
fi
export PACKAGER_PRIVKEY="$(ls "$KEY_DIR"/*.rsa | head -n1)"
# Make the public key available to apk so locally built deps can be installed
cp "$KEY_DIR"/*.pub /etc/apk/keys/ 2>/dev/null || true

# ── Step 4: Generate checksums if SKIP placeholder is still set ──────────────
echo "[4/7] Generating checksums..."
for pkg in baltek-dte-config baltek-vos-ui baltek-dte; do
  APKBUILD="$APORTS_DIR/$pkg/APKBUILD"
  if grep -q 'sha512sums="SKIP"' "$APKBUILD" 2>/dev/null; then
    echo "  Generating checksums for $pkg..."
    ( cd "$APORTS_DIR/$pkg" && abuild checksum )
  fi
done

# ── Step 5: Build packages ────────────────────────────────────────────────────
export APORTSDIR="$APORTS_DIR"
export REPODEST="$OUT_DIR"

echo "[5/7] Building packages..."
for pkg in baltek-dte-config baltek-vos-ui baltek-dte; do
  echo "  Building $pkg..."
  ( cd "$APORTS_DIR/$pkg" && abuild -r )
done

# ── Step 6: Index and sign the repo ──────────────────────────────────────────
echo "[6/7] Indexing repo..."
apk index \
  --output "$OUT_DIR/${REPO_NAME}.INDEX.tar.gz" \
  --rewrite-arch "$(apk --print-arch)" \
  "$OUT_DIR"/*.apk
abuild-sign "$OUT_DIR/${REPO_NAME}.INDEX.tar.gz"

# ── Step 7: Done ─────────────────────────────────────────────────────────────
echo "[7/7] Done."
echo ""
echo "Repo output:  $OUT_DIR"
echo "Public key:   $(ls "$KEY_DIR"/*.pub)"
echo ""
echo "To use this repo on Alpine:"
echo "  1. Host $OUT_DIR over HTTP (nginx, GitHub Pages, S3, etc.)"
echo "  2. Copy the .pub key to /etc/apk/keys/ on each target machine"
echo "  3. echo '<repo-url>' >> /etc/apk/repositories"
echo "  4. apk update && apk add baltek-dte"
