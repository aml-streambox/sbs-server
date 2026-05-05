#!/bin/bash
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SDK_ROOT_DEFAULT="$ROOT_DIR/.sdk/toolchain/sbs-sdk-mesont7c-kvim4-5.15"
SDK_ROOT="${SBS_SDK_ROOT:-$SDK_ROOT_DEFAULT}"

if [ ! -d "$SDK_ROOT" ]; then
    echo "SBS SDK not found at: $SDK_ROOT" >&2
    echo "Run scripts/setup-sdk.sh after generating the SDK from Yocto." >&2
    return 1 2>/dev/null || exit 1
fi

ENV_SCRIPT=$(find "$SDK_ROOT" -maxdepth 1 -type f -name 'environment-setup-*' | head -n 1)
if [ -z "$ENV_SCRIPT" ]; then
    echo "No environment-setup script found under: $SDK_ROOT" >&2
    return 1 2>/dev/null || exit 1
fi

# shellcheck disable=SC1090
. "$ENV_SCRIPT"

export SBS_SDK_ROOT="$SDK_ROOT"
export SBS_SDK_ENV_SCRIPT="$ENV_SCRIPT"
