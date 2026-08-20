#!/usr/bin/env bash
#
# flash-local.sh — the ONE way to flash this board locally.
#
# Usage:
#   ./flash-local.sh              # fetch creds from VPS, build, flash, verify
#   ./flash-local.sh --monitor    # ...then tail the serial log
#   ./flash-local.sh --baseline <git-ref>   # build+flash a specific commit (A/B testing)
#   ./flash-local.sh --no-creds   # deliberately flash WITHOUT MQTT creds
#
# WHY THIS SCRIPT EXISTS (2026-08-20 incident — read before "simplifying" it):
#
#   1. CREDENTIALS ARE COMPILE-TIME. MQTT_USERNAME/PASSWORD and
#      FIRMWARE_UPLOAD_TOKEN are baked in via -D at build time. `make flash`
#      re-triggers a build, so if you don't pass them on the SAME invocation,
#      the ?= defaults (empty) win and you silently flash a board that joins
#      WiFi fine and is then rejected by the broker. Easy to misdiagnose for
#      an hour. This script always fetches them and always passes them.
#
#   2. SWAP_BANK SILENTLY DEFEATS THE FLASH. This board does dual-bank OTA. A
#      board that has ever taken an OTA has SWAP_BANK=1, so what the CPU boots
#      and what `-w 0x08000000` writes can disagree — the programmer reports
#      "Download verified successfully" and the board keeps running the OLD
#      firmware. Cost a lot of confusion today: the fix appeared not to work
#      when it had simply never been executed. This script always checks
#      SWAP_BANK first and clears it, so flashing is deterministic.
#
#   3. FLASHING DOES NOT ERASE WIFI CREDENTIALS. The app image is ~135KB
#      (~17 pages); stored WiFi creds live near the top of the bank (page
#      126/127). Verified safe — the board keeps its network after a reflash.
#
# NOTE: drag-and-drop onto the E:/DIS_U585AI mass-storage volume DOES NOT WORK
# on this board (fails with "Flash algorithm write command FAILURE"). Use SWD
# via STM32_Programmer_CLI, which is what this script does.

set -euo pipefail

FIRMWARE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CUBE_CLI="/c/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe"
VPS="${HESPERUS_VPS:-root@46.62.200.8}"
ENV_PROD="/opt/hesperus/.env.prod"

MONITOR=0
NO_CREDS=0
BASELINE_REF=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --monitor)  MONITOR=1; shift ;;
        --no-creds) NO_CREDS=1; shift ;;
        --baseline) BASELINE_REF="${2:-}"; shift 2 ;;
        -h|--help)  sed -n '2,30p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ ! -x "$CUBE_CLI" ]]; then
    echo "FATAL: STM32_Programmer_CLI not found at:" >&2
    echo "  $CUBE_CLI" >&2
    echo "Install STM32CubeProgrammer, or set CUBE_CLI in this script." >&2
    exit 1
fi

# ── Step 1: credentials ───────────────────────────────────────────────
MQTT_U=""; MQTT_P=""; FW_T=""
if [[ $NO_CREDS -eq 0 ]]; then
    echo "==> Fetching build credentials from $VPS:$ENV_PROD"
    ENVDATA="$(ssh -o ConnectTimeout=10 "$VPS" \
        "grep -E '^(MQTT_USERNAME|MQTT_PASSWORD|FIRMWARE_UPLOAD_TOKEN)=' $ENV_PROD")"
    MQTT_U="$(echo "$ENVDATA" | grep '^MQTT_USERNAME='        | cut -d= -f2- | tr -d '"'\''\r')"
    MQTT_P="$(echo "$ENVDATA" | grep '^MQTT_PASSWORD='        | cut -d= -f2- | tr -d '"'\''\r')"
    FW_T="$(  echo "$ENVDATA" | grep '^FIRMWARE_UPLOAD_TOKEN=' | cut -d= -f2- | tr -d '"'\''\r')"

    if [[ -z "$MQTT_U" || -z "$MQTT_P" ]]; then
        echo "FATAL: credentials came back empty — refusing to flash a board that" >&2
        echo "       would join WiFi and then be rejected by the broker." >&2
        exit 1
    fi
    # Lengths only — never print secrets.
    echo "    ok (user ${#MQTT_U} chars, pass ${#MQTT_P} chars, token ${#FW_T} chars)"
else
    echo "==> --no-creds: building WITHOUT MQTT credentials (broker auth will fail)"
fi

# ── Step 2: pick source tree ──────────────────────────────────────────
BUILD_DIR="$FIRMWARE_DIR"
WORKTREE=""
if [[ -n "$BASELINE_REF" ]]; then
    WORKTREE="$(mktemp -d -t hesperus-baseline-XXXXXX)"
    echo "==> Building baseline ref '$BASELINE_REF' in $WORKTREE"
    git -C "$FIRMWARE_DIR/.." worktree add --detach "$WORKTREE" "$BASELINE_REF" >/dev/null
    BUILD_DIR="$WORKTREE/firmware"
    cleanup() { git -C "$FIRMWARE_DIR/.." worktree remove "$WORKTREE" --force >/dev/null 2>&1 || true; }
    trap cleanup EXIT
fi

# ── Step 3: clear SWAP_BANK (see header note 2) ───────────────────────
echo "==> Checking SWAP_BANK option byte"
SWAP="$("$CUBE_CLI" -c port=SWD -ob displ 2>/dev/null | grep -i 'SWAP_BANK' | head -1 || true)"
echo "    $SWAP"
if echo "$SWAP" | grep -q '0x1'; then
    echo "    SWAP_BANK=1 (board has taken an OTA) — clearing so the flash is deterministic"
    "$CUBE_CLI" -c port=SWD -ob SWAP_BANK=0 >/dev/null
    echo "    cleared"
fi

# ── Step 4: build + flash ─────────────────────────────────────────────
echo "==> Building and flashing"
make -C "$BUILD_DIR" flash \
    MQTT_USERNAME="$MQTT_U" \
    MQTT_PASSWORD="$MQTT_P" \
    FIRMWARE_UPLOAD_TOKEN="$FW_T" 2>&1 \
  | grep -iE 'flash budget|download|verif|error|reset|warning: flashing' || true

echo "==> Done. Board reset into the new firmware."

# ── Step 5: optional serial monitor ───────────────────────────────────
if [[ $MONITOR -eq 1 ]]; then
    echo "==> Monitoring COM7 @115200 (Ctrl-C to stop)"
    exec "$FIRMWARE_DIR/monitor-serial.sh"
fi
