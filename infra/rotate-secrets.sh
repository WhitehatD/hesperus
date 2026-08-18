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
#   ANTHROPIC_API_KEY=... FIRMWARE_UPLOAD_TOKEN=... GEMINI_API_KEY=... \
#   MQTT_USERNAME=hesperus-board MQTT_PASSWORD=... \
#   bash infra/rotate-secrets.sh

set -euo pipefail

DEPLOY_DIR="/opt/hesperus"
ENV_FILE="$DEPLOY_DIR/.env.prod"

: "${FIRMWARE_UPLOAD_TOKEN:?required}"
: "${MQTT_USERNAME:?required}"
: "${MQTT_PASSWORD:?required}"
# ANTHROPIC_API_KEY / GEMINI_API_KEY are deliberately NOT required (2026-08-18
# decision: moving to OpenRouter — see TODO in server/app/config.py). Left
# empty here means AI analysis won't work until that migration lands, but
# infra (containers, MQTT auth, upload auth, dashboard reachability) is
# independently verifiable without them.

printf '%s\n' \
  "ANTHROPIC_API_KEY=${ANTHROPIC_API_KEY:-}" \
  "FIRMWARE_UPLOAD_TOKEN=${FIRMWARE_UPLOAD_TOKEN}" \
  "GEMINI_API_KEY=${GEMINI_API_KEY:-}" \
  "MQTT_USERNAME=${MQTT_USERNAME}" \
  "MQTT_PASSWORD=${MQTT_PASSWORD}" \
  > "$ENV_FILE"
chown hesperus-deploy:hesperus-deploy "$ENV_FILE"
chmod 600 "$ENV_FILE"
echo "wrote $ENV_FILE"

docker run --rm -v "$DEPLOY_DIR/mosquitto:/mosquitto/config" eclipse-mosquitto:2 \
  mosquitto_passwd -b -c /mosquitto/config/passwd "$MQTT_USERNAME" "$MQTT_PASSWORD"
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

echo "Done. Next deploy (push to main, or: sudo -u hesperus-deploy /opt/hesperus/infra/deploy.sh)"
echo "will pick these up. mosquitto itself is NOT restarted by this script or by deploy.sh —"
echo "restart it manually if the MQTT password actually changed:"
echo "  cd $DEPLOY_DIR && docker compose -f docker-compose.yml -f docker-compose.prod.yml up -d --force-recreate mosquitto"
