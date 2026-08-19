#!/usr/bin/env bash
# Hesperus secret rotation — run as ROOT on infra-core (46.62.200.8) whenever
# a secret actually changes. NOT part of the CI hot path — deploy.sh (run by
# the restricted hesperus-deploy user on every push) only READS the files
# this script writes, it never writes them itself.
#
# Why secrets don't flow through CI -> SSH -> deploy.sh: verified 2026-08-18
# that appleboy/ssh-action's env-passing does not survive the sshd
# ForceCommand on the hesperus-deploy user (see infra/deploy.sh header for
# the full root-cause). Rather than fight that, secrets now live in a
# persistent root-owned file, rotated out-of-band. This is a smaller blast
# radius anyway: DEPLOY_SSH_KEY (the CI-facing key) carries zero secrets.
#
# Usage (as root on infra-core):
#   OPENROUTER_API_KEY=... FIRMWARE_UPLOAD_TOKEN=... \
#   MQTT_USERNAME=hesperus-board MQTT_PASSWORD=... \
#   DASHBOARD_MQTT_USERNAME=hesperus-dashboard DASHBOARD_MQTT_PASSWORD=... \
#   bash infra/rotate-secrets.sh
#
# ANTHROPIC_API_KEY/GEMINI_API_KEY are optional passthrough, kept only for
# manual model_key testing — OPENROUTER_API_KEY is REQUIRED and is what the
# production chat/planning/analysis paths actually use as of 2026-08-19.
#
# DASHBOARD_MQTT_* is a SEPARATE, read-only identity (mosquitto/acl) for the
# browser client — never reuse MQTT_USERNAME/PASSWORD (the board's readwrite
# creds) for it. Optional but strongly recommended: without it the dashboard
# shows no live telemetry (broker rejects anonymous WS clients).

set -euo pipefail

DEPLOY_DIR="/opt/hesperus"
ENV_FILE="$DEPLOY_DIR/.env.prod"

: "${OPENROUTER_API_KEY:?required}"
: "${FIRMWARE_UPLOAD_TOKEN:?required}"
: "${MQTT_USERNAME:?required}"
: "${MQTT_PASSWORD:?required}"
# ANTHROPIC_API_KEY / GEMINI_API_KEY are deliberately NOT required (2026-08-19:
# OpenRouter migration landed — see server/app/config.py). They're optional
# passthrough kept only for manual model_key testing; production chat,
# schedule planning, and image analysis all use OPENROUTER_API_KEY.

printf '%s\n' \
  "OPENROUTER_API_KEY=${OPENROUTER_API_KEY}" \
  "ANTHROPIC_API_KEY=${ANTHROPIC_API_KEY:-}" \
  "FIRMWARE_UPLOAD_TOKEN=${FIRMWARE_UPLOAD_TOKEN}" \
  "GEMINI_API_KEY=${GEMINI_API_KEY:-}" \
  "MQTT_USERNAME=${MQTT_USERNAME}" \
  "MQTT_PASSWORD=${MQTT_PASSWORD}" \
  > "$ENV_FILE"
chown hesperus-deploy:hesperus-deploy "$ENV_FILE"
chmod 600 "$ENV_FILE"
echo "wrote $ENV_FILE"

# mosquitto 2.x's -c REFUSES to clobber an existing file ("Error: Unable to
# open file ... for writing. File exists.") rather than truncating it, and
# --user root does NOT get you around it — it's an explicit guard, not a
# permission problem. Since a rotation regenerates every identity anyway,
# remove it first and build the file from scratch.
rm -f "$DEPLOY_DIR/mosquitto/passwd"

# -c CREATES, so it must be used for the FIRST user only; the second call
# omits it to APPEND. Getting this backwards silently leaves the broker with
# only one valid identity.
docker run --rm --user root -v "$DEPLOY_DIR/mosquitto:/mosquitto/config" eclipse-mosquitto:2 \
  mosquitto_passwd -b -c /mosquitto/config/passwd "$MQTT_USERNAME" "$MQTT_PASSWORD"

# Second identity: the browser dashboard (dashboard/app/hooks/useMQTT.ts).
# Separate user because its credentials ship inside the public JS bundle —
# mosquitto/acl restricts it to `topic read`, so a leak can only watch
# telemetry, never publish a board command. Giving the browser the board's
# own readwrite credentials instead would re-open the command-injection hole
# closed on 2026-08-18.
if [ -n "${DASHBOARD_MQTT_USERNAME:-}" ] && [ -n "${DASHBOARD_MQTT_PASSWORD:-}" ]; then
  docker run --rm --user root -v "$DEPLOY_DIR/mosquitto:/mosquitto/config" eclipse-mosquitto:2 \
    mosquitto_passwd -b /mosquitto/config/passwd "$DASHBOARD_MQTT_USERNAME" "$DASHBOARD_MQTT_PASSWORD"
  echo "added dashboard MQTT user '$DASHBOARD_MQTT_USERNAME' (read-only per mosquitto/acl)"
else
  echo "WARNING: DASHBOARD_MQTT_USERNAME/PASSWORD unset — the dashboard's live" >&2
  echo "  MQTT feed will show nothing (broker rejects anonymous WS clients)." >&2
fi

# Must be owned by the mosquitto user INSIDE the container (uid/gid 1883),
# not by hesperus-deploy. The broker drops privileges to that user and reads
# the pwfile as it — with hesperus-deploy:600 it fails to start entirely
# ("password-file: Error: Unable to open pwfile"), which then shows up as an
# unhealthy container and a failed deploy. deploy.sh only does a `-f`
# existence test on this path, which needs directory traversal, not file
# read, so it doesn't need to own or read it.
chown 1883:1883 "$DEPLOY_DIR/mosquitto/passwd"
chmod 600 "$DEPLOY_DIR/mosquitto/passwd"
echo "wrote $DEPLOY_DIR/mosquitto/passwd (owned by container uid 1883)"

# ACL is tracked in git (no secrets in it) but must be readable by uid 1883.
if [ -f "$DEPLOY_DIR/mosquitto/acl" ]; then
  chown 1883:1883 "$DEPLOY_DIR/mosquitto/acl"
  chmod 644 "$DEPLOY_DIR/mosquitto/acl"
  echo "fixed ownership on $DEPLOY_DIR/mosquitto/acl"
fi

echo "Done. Next deploy (push to main, or: sudo -u hesperus-deploy /opt/hesperus/infra/deploy.sh)"
echo "will pick these up. mosquitto itself is NOT restarted by this script or by deploy.sh —"
echo "restart it manually if the MQTT password actually changed:"
echo "  cd $DEPLOY_DIR && docker compose -f docker-compose.yml -f docker-compose.prod.yml up -d --force-recreate mosquitto"
