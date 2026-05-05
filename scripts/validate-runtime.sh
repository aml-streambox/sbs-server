#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -f "$ROOT_DIR/.env" ]; then
    set -a
    # shellcheck disable=SC1091
    . "$ROOT_DIR/.env"
    set +a
fi

TARGET="${1:-${TARGET:-}}"
if [ -z "$TARGET" ]; then
    echo "Set TARGET in the environment or .env (for example: TARGET=root@device-hostname.local)." >&2
    exit 1
fi

API_PORT="${SBS_API_PORT:-10100}"
OUTPUT_ID="${SBS_OUTPUT_ID:-default-out}"
SOURCE_ID="${SBS_SOURCE_ID:-default-src}"
TARGET_HOST="${TARGET#*@}"
SBS_CLIENT_CLI="${SBS_CLIENT_CLI:-sbs-client-cli}"

echo "[1/6] Checking service state on ${TARGET}"
ssh "$TARGET" "systemctl is-active sbs-server && systemctl --no-pager --full status sbs-server | sed -n '1,12p'"

echo "[2/6] Capturing initial runtime state"
"$SBS_CLIENT_CLI" --host "$TARGET_HOST" --port "$API_PORT" --json system state

echo "[3/6] Restarting output worker via API-compatible workflow"
if "$SBS_CLIENT_CLI" --host "$TARGET_HOST" --port "$API_PORT" --json rpc output.stop "{\"id\":\"${OUTPUT_ID}\"}"; then
  "$SBS_CLIENT_CLI" --host "$TARGET_HOST" --port "$API_PORT" --json rpc output.start "{\"id\":\"${OUTPUT_ID}\"}"
else
  echo "Output ${OUTPUT_ID} not present; exercising source restart instead"
  "$SBS_CLIENT_CLI" --host "$TARGET_HOST" --port "$API_PORT" --json rpc source.stop "{\"id\":\"${SOURCE_ID}\"}"
  "$SBS_CLIENT_CLI" --host "$TARGET_HOST" --port "$API_PORT" --json rpc source.start "{\"id\":\"${SOURCE_ID}\"}"
fi

echo "[4/6] Checking source/output worker processes"
ssh "$TARGET" "ps -ef | grep '[s]bs-worker'"

echo "[5/6] Gathering runtime health after restart exercise"
"$SBS_CLIENT_CLI" --host "$TARGET_HOST" --port "$API_PORT" --json system info

echo "[6/6] Recent service log excerpt"
ssh "$TARGET" "journalctl -u sbs-server -n 40 --no-pager"
