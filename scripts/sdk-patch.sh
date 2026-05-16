#!/bin/bash
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -f "$ROOT_DIR/.env" ]; then
    set -a
    # shellcheck disable=SC1091
    . "$ROOT_DIR/.env"
    set +a
fi

YOCTO_ROOT="${YOCTO_ROOT:-}"
SDK_ROOT="${1:-${SBS_SDK_ROOT:-$ROOT_DIR/.sdk/toolchain/sbs-sdk-mesont7c-kvim4-5.15}}"
TARGET_SYSROOT="$SDK_ROOT/sysroots/armv8a-poky-linux"
SRC_HEADERS=""
if [ -n "$YOCTO_ROOT" ]; then
    SRC_HEADERS="$YOCTO_ROOT/build/tmp/sysroots-components/armv8a/vulkan-headers/usr/include/vulkan"
fi
PC_DIR="$TARGET_SYSROOT/usr/lib/pkgconfig"

if [ ! -d "$TARGET_SYSROOT" ]; then
    echo "Target sysroot not found under SDK: $TARGET_SYSROOT" >&2
    exit 1
fi

# Some Amlogic SDK exports include libvulkan but omit headers/pkg-config
# metadata, so patch only the missing pieces needed by Meson discovery.
if [ -n "$SRC_HEADERS" ] && [ -d "$SRC_HEADERS" ] && [ ! -d "$TARGET_SYSROOT/usr/include/vulkan" ]; then
    mkdir -p "$TARGET_SYSROOT/usr/include"
    cp -a "$SRC_HEADERS" "$TARGET_SYSROOT/usr/include/"
fi

if [ ! -f "$PC_DIR/vulkan.pc" ]; then
    mkdir -p "$PC_DIR"
    cat > "$PC_DIR/vulkan.pc" <<'EOF'
prefix=/usr
exec_prefix=${prefix}
libdir=${exec_prefix}/lib
includedir=${prefix}/include

Name: Vulkan
Description: Vulkan loader
Version: 1.2.196
Libs: -L${libdir} -lvulkan
Cflags: -I${includedir}
EOF
fi

echo "Patched SDK Vulkan metadata in: $SDK_ROOT"
