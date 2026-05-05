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
SDK_NAME="${SDK_NAME:-sbs-sdk-mesont7c-kvim4-5.15.sh}"
SDK_SRC_DEFAULT=""
if [ -n "$YOCTO_ROOT" ]; then
    SDK_SRC_DEFAULT="$YOCTO_ROOT/build/tmp/deploy/sdk/$SDK_NAME"
fi
SDK_SRC="${SDK_SRC:-$SDK_SRC_DEFAULT}"
SDK_INSTALL_ROOT="${SDK_INSTALL_ROOT:-$ROOT_DIR/.sdk/toolchain/sbs-sdk-mesont7c-kvim4-5.15}"
SDK_INSTALLERS_DIR="$ROOT_DIR/.sdk/installers"

mkdir -p "$SDK_INSTALLERS_DIR" "$ROOT_DIR/.sdk/toolchain"

if [ -z "$SDK_SRC" ]; then
    echo "Set SDK_SRC or YOCTO_ROOT in the environment or .env." >&2
    exit 1
fi

if [ ! -f "$SDK_SRC" ]; then
    echo "SDK installer not found: $SDK_SRC" >&2
    echo "Generate it with: bitbake sbs-sdk" >&2
    exit 1
fi

cp "$SDK_SRC" "$SDK_INSTALLERS_DIR/"
sh "$SDK_INSTALLERS_DIR/$SDK_NAME" -y -d "$SDK_INSTALL_ROOT"

"$ROOT_DIR/scripts/sdk-patch.sh" "$SDK_INSTALL_ROOT"

echo "SDK installed to: $SDK_INSTALL_ROOT"
echo "Source it with: source scripts/sdk-env.sh"
