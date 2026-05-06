#!/bin/bash
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -f "$ROOT_DIR/.env" ]; then
    set -a
    # shellcheck disable=SC1091
    . "$ROOT_DIR/.env"
    set +a
fi

BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-sdk}"
ARTIFACT_DIR="${ARTIFACT_DIR:-$ROOT_DIR/artifacts/sdk-package}"
GENERATED_CROSS_DIR="$ROOT_DIR/cross/generated"
GENERATED_CROSS_FILE="$GENERATED_CROSS_DIR/aarch64-yocto-sdk.cross"

# shellcheck disable=SC1091
source "$ROOT_DIR/scripts/sdk-env.sh"

mkdir -p "$BUILD_DIR" "$ARTIFACT_DIR" "$GENERATED_CROSS_DIR"

python3 - <<'PY' > "$GENERATED_CROSS_FILE"
import os, shlex

def arr(name, fallback=None):
    value = os.environ.get(name, fallback or '')
    parts = shlex.split(value)
    return '[' + ', '.join(repr(p) for p in parts) + ']'

target_arch = os.environ.get('OECORE_TARGET_ARCH', 'aarch64')
cpu_family = 'aarch64' if 'aarch64' in target_arch or 'armv8' in target_arch else target_arch

print('[binaries]')
print(f"c = {arr('CC')}")
print(f"cpp = {arr('CXX')}")
print(f"ar = {arr('AR', 'ar')}")
print(f"strip = {arr('STRIP', 'strip')}")
print(f"pkgconfig = {arr('PKG_CONFIG', 'pkg-config')}")
print('')
print('[host_machine]')
print("system = 'linux'")
print(f"cpu_family = '{cpu_family}'")
print(f"cpu = '{target_arch}'")
print("endian = 'little'")
PY

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    meson setup "$BUILD_DIR" \
        --cross-file "$GENERATED_CROSS_FILE" \
        --prefix /usr \
        --sysconfdir /etc \
        --localstatedir /var \
        -Dplatform=amlogic \
        -Dshader_compile=true \
        -Dtests=false
else
    meson setup "$BUILD_DIR" --reconfigure --cross-file "$GENERATED_CROSS_FILE" --prefix /usr --sysconfdir /etc --localstatedir /var -Dshader_compile=true
fi

meson compile -C "$BUILD_DIR"

rm -rf "$ARTIFACT_DIR/rootfs"
DESTDIR="$ARTIFACT_DIR/rootfs" meson install -C "$BUILD_DIR" --no-rebuild

install -d "$ARTIFACT_DIR/rootfs/etc/systemd/system"
install -m 0644 "$ROOT_DIR/data/sbs-server.service" "$ARTIFACT_DIR/rootfs/etc/systemd/system/"

install -d "$ARTIFACT_DIR/rootfs/etc/tmpfiles.d"
install -m 0644 "$ROOT_DIR/data/sbs-tmpfiles.conf" "$ARTIFACT_DIR/rootfs/etc/tmpfiles.d/sbs.conf"

cat > "$ARTIFACT_DIR/manifest.txt" <<EOF
SDK_ROOT=$SBS_SDK_ROOT
SDK_ENV_SCRIPT=$SBS_SDK_ENV_SCRIPT
BUILD_DIR=$BUILD_DIR
ROOTFS_DIR=$ARTIFACT_DIR/rootfs
EOF

tar -C "$ARTIFACT_DIR/rootfs" -czf "$ARTIFACT_DIR/sbs-sdk-package.tar.gz" .

echo "SDK build complete: $ARTIFACT_DIR/rootfs"
