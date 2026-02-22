#!/bin/bash
# scripts/build-rootfs.sh - Build Alpine Linux rootfs for Linux AI OS
#
# Downloads Alpine Linux minirootfs and installs the packages needed
# to run aicore + DTE. Produces rootfs/ directory ready for initramfs.
#
# Usage:
#   ./scripts/build-rootfs.sh [--alpine-version 3.21] [--arch x86_64]
#
# Requirements (on build host):
#   - wget or curl
#   - tar, gzip
#   - sudo (for chroot / apk)
#   - qemu-user-static (if cross-compiling)
#
# The resulting rootfs/ is used by:
#   make INITRAMFS_SOURCE=rootfs/ isoimage
#
# To add your API key after build:
#   echo '"api_key": "sk-ant-..."' >> rootfs/etc/aicore/config.json
#   (or edit rootfs/etc/aicore/config.json)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ALPINE_VERSION="${1:-3.21}"
ARCH="${2:-x86_64}"
ROOTFS_DIR="$REPO_ROOT/rootfs"
WORK_DIR="$REPO_ROOT/.build/rootfs-work"
ALPINE_MIRROR="https://dl-cdn.alpinelinux.org/alpine"

# Packages to install into rootfs
ALPINE_PACKAGES=(
    # Core userspace
    busybox
    busybox-extras
    musl
    musl-utils

    # Network (for DHCP + aicore HTTP)
    ca-certificates
    curl
    libcurl
    openssl

    # ncurses (for DTE)
    ncurses
    ncurses-libs

    # Runtime libs
    libgcc
    libstdc++

    # Init helpers
    openrc
    util-linux
)

echo "=== Linux AI OS rootfs builder ==="
echo "Alpine: $ALPINE_VERSION / $ARCH"
echo "Output: $ROOTFS_DIR"
echo ""

mkdir -p "$WORK_DIR"
cd "$WORK_DIR"

# Download Alpine minirootfs
TARBALL="alpine-minirootfs-${ALPINE_VERSION}.0-${ARCH}.tar.gz"
TARBALL_URL="${ALPINE_MIRROR}/v${ALPINE_VERSION}/releases/${ARCH}/${TARBALL}"

if [ ! -f "$TARBALL" ]; then
    echo "[1/5] Downloading Alpine $ALPINE_VERSION minirootfs..."
    wget -q --show-progress "$TARBALL_URL" -O "$TARBALL" || \
        curl -L --progress-bar "$TARBALL_URL" -o "$TARBALL"
else
    echo "[1/5] Using cached $TARBALL"
fi

# Extract into rootfs
echo "[2/5] Extracting minirootfs..."
mkdir -p "$ROOTFS_DIR"
# Preserve our skeleton files - only extract Alpine base
sudo tar xzf "$TARBALL" -C "$ROOTFS_DIR" \
    --exclude="./etc/aicore" \
    --skip-old-files \
    2>/dev/null || tar xzf "$TARBALL" -C "$ROOTFS_DIR"

# Configure APK repositories
echo "[3/5] Configuring APK repositories..."
cat > "$ROOTFS_DIR/etc/apk/repositories" <<EOF
${ALPINE_MIRROR}/v${ALPINE_VERSION}/main
${ALPINE_MIRROR}/v${ALPINE_VERSION}/community
EOF

# Install packages via chroot (requires root)
echo "[4/5] Installing packages: ${ALPINE_PACKAGES[*]}"
if command -v sudo >/dev/null 2>&1; then
    sudo chroot "$ROOTFS_DIR" /bin/sh -c "
        apk update && \
        apk add --no-cache ${ALPINE_PACKAGES[*]}
    "
else
    echo "[warn] Not running as root - skipping package install."
    echo "       Run: sudo chroot $ROOTFS_DIR apk add ${ALPINE_PACKAGES[*]}"
fi

# Install our AI OS files
echo "[5/5] Installing Linux AI OS components..."

# init script (already in rootfs/ from git)
chmod +x "$ROOTFS_DIR/init" 2>/dev/null || true

# Create placeholder binaries (replaced by actual build)
if [ ! -x "$ROOTFS_DIR/usr/sbin/aicore" ]; then
    echo "[warn] aicore binary not found at $ROOTFS_DIR/usr/sbin/aicore"
    echo "       Build it with: cd tools/aicore && make && cp aicore $ROOTFS_DIR/usr/sbin/"
fi

if [ ! -x "$ROOTFS_DIR/usr/bin/dte" ]; then
    echo "[warn] dte binary not found at $ROOTFS_DIR/usr/bin/dte"
    echo "       Build it with: cd tools/dte && make && cp dte $ROOTFS_DIR/usr/bin/"
fi

# Ensure /etc/aicore/config.json exists
mkdir -p "$ROOTFS_DIR/etc/aicore"
if [ ! -f "$ROOTFS_DIR/etc/aicore/config.json" ]; then
    cp "$REPO_ROOT/rootfs/etc/aicore/config.json" \
       "$ROOTFS_DIR/etc/aicore/config.json" 2>/dev/null || true
fi

# Set up /etc/passwd, /etc/group (minimal)
if [ ! -f "$ROOTFS_DIR/etc/passwd" ]; then
    cat > "$ROOTFS_DIR/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/sh
nobody:x:65534:65534:nobody:/:/sbin/nologin
EOF
fi

if [ ! -f "$ROOTFS_DIR/etc/group" ]; then
    cat > "$ROOTFS_DIR/etc/group" <<'EOF'
root:x:0:root
nobody:x:65534:
EOF
fi

# /etc/hostname
echo "ai-os" > "$ROOTFS_DIR/etc/hostname"

# /etc/hosts
cat > "$ROOTFS_DIR/etc/hosts" <<'EOF'
127.0.0.1   localhost ai-os
::1         localhost ai-os
EOF

# Ensure /etc/resolv.conf works in initramfs
cat > "$ROOTFS_DIR/etc/resolv.conf" <<'EOF'
nameserver 1.1.1.1
nameserver 8.8.8.8
EOF

echo ""
echo "=== rootfs build complete ==="
echo ""
echo "Rootfs size: $(du -sh "$ROOTFS_DIR" 2>/dev/null | cut -f1)"
echo ""
echo "Next steps:"
echo "  1. Build aicore:  cd tools/aicore && make CC=musl-gcc && cp aicore $ROOTFS_DIR/usr/sbin/"
echo "  2. Build dte:     cd tools/dte    && make CC=musl-gcc && cp dte $ROOTFS_DIR/usr/bin/"
echo "  3. Set API key:   edit $ROOTFS_DIR/etc/aicore/config.json"
echo "  4. Build ISO:     make ai_defconfig && make INITRAMFS_SOURCE=rootfs isoimage -j\$(nproc)"
echo "  5. Test in QEMU:  qemu-system-x86_64 -cdrom arch/x86/boot/image.iso -m 512M -nographic"
echo ""
