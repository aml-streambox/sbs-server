#!/bin/bash
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -f "$ROOT_DIR/.env" ]; then
    set -a
    # shellcheck disable=SC1091
    . "$ROOT_DIR/.env"
    set +a
fi

TARGET="${TARGET:-}"
if [ -z "$TARGET" ]; then
    echo "Set TARGET in the environment or .env (for example: TARGET=root@device-hostname.local)." >&2
    exit 1
fi

BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-sdk}"
ARTIFACT_DIR="${ARTIFACT_DIR:-$ROOT_DIR/artifacts/sdk-package/rootfs}"
REMOTE_BINDIR="/usr/bin"
REMOTE_SHADIR="/usr/share/sbs/shaders"
REMOTE_UNITDIR="/etc/systemd/system"

msg() { printf "\033[1;34m[SBS-DEPLOY]\033[0m %s\n" "$*"; }

msg "Cross-compiling with workspace Yocto SDK..."
"$ROOT_DIR/scripts/build-sdk.sh"

msg "Deploying binaries to ${TARGET}..."
ssh "${TARGET}" "systemctl stop sbs-server || true"
scp "${ARTIFACT_DIR}${REMOTE_BINDIR}/sbs-server" "${TARGET}:${REMOTE_BINDIR}/"
scp "${ARTIFACT_DIR}${REMOTE_BINDIR}/sbs-worker" "${TARGET}:${REMOTE_BINDIR}/"
scp "${ARTIFACT_DIR}${REMOTE_BINDIR}/sbs-cli" "${TARGET}:${REMOTE_BINDIR}/"

msg "Deploying shaders to ${TARGET}..."
ssh "${TARGET}" "mkdir -p ${REMOTE_SHADIR}"
shopt -s nullglob
shader_files=("${ARTIFACT_DIR}${REMOTE_SHADIR}"/*.spv)
if [ ${#shader_files[@]} -gt 0 ]; then
    scp "${shader_files[@]}" "${TARGET}:${REMOTE_SHADIR}/"
fi
shopt -u nullglob

msg "Deploying systemd unit..."
scp "${ARTIFACT_DIR}${REMOTE_UNITDIR}/sbs-server.service" "${TARGET}:${REMOTE_UNITDIR}/"
ssh "${TARGET}" "systemctl daemon-reload"

msg "Restarting sbs-server..."
ssh "${TARGET}" "systemctl restart sbs-server"

msg "Waiting for service..."
sleep 2
ssh "${TARGET}" "systemctl status sbs-server --no-pager" || true

msg "Deploy complete."
