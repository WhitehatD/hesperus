#!/usr/bin/env bash
# Hesperus deploy script — runs ON infra-core (46.62.200.8) as the restricted
# `hesperus-deploy` system user, invoked via an SSH forced command (sshd_config
# Match block: ForceCommand /opt/hesperus/infra/deploy.sh — see
# vps-provision.sh; not an authorized_keys command= prefix).
#
# Because it's a forced command, whatever "script" text the CI client sends is
# IGNORED — this script always runs instead, regardless of what ci.yml's
# `script:` field contains. Env vars still reach it via SSH's AcceptEnv
# mechanism (see infra/vps-provision.sh's sshd Match block), independent of
# the forced command.
#
# Deliberately hard-scoped to /opt/hesperus — no `find` across the
# filesystem, no clone-fallback, no touching anything outside this directory.
# This is the guest-tenant-owns-its-own-infra-as-code design (see plan
# discussion 2026-08-18): infra-core is a shared box, this script must never
# be able to reach Forgejo/ntfy/Umami/oauth2-proxy/etc.

set -euo pipefail

DEPLOY_DIR="/opt/hesperus"
cd "$DEPLOY_DIR"

echo "== Hesperus deploy: $(date -u +%FT%TZ) =="

git fetch origin main
git reset --hard origin/main

# Inject secrets into the persistent VPS env file (overwritten every deploy —
# GitHub Secrets are the source of truth, never hand-edit .env.prod on the box).
ENV_FILE="$DEPLOY_DIR/.env.prod"
printf '%s\n' \
  "ANTHROPIC_API_KEY=${ANTHROPIC_API_KEY:-}" \
  "FIRMWARE_UPLOAD_TOKEN=${FIRMWARE_UPLOAD_TOKEN:-}" \
  "GEMINI_API_KEY=${GEMINI_API_KEY:-}" \
  "MQTT_USERNAME=${MQTT_USERNAME:-}" \
  "MQTT_PASSWORD=${MQTT_PASSWORD:-}" \
  > "$ENV_FILE"
chmod 600 "$ENV_FILE"

set -a
# shellcheck disable=SC1090
source "$ENV_FILE"
set +a

# Regenerate mosquitto's password file from the current secrets. Idempotent
# and harmless to run every deploy — docker compose only restarts a service
# whose OWN compose-file config changed, not when a bind-mounted file's
# CONTENTS change, so this does NOT restart mosquitto (see the "leave
# mosquitto alone" note below — that guarantee still holds).
if [ -n "${MQTT_USERNAME:-}" ] && [ -n "${MQTT_PASSWORD:-}" ]; then
  docker run --rm -v "$DEPLOY_DIR/mosquitto:/mosquitto/config" eclipse-mosquitto:2 \
    mosquitto_passwd -b -c /mosquitto/config/passwd "$MQTT_USERNAME" "$MQTT_PASSWORD"
  chmod 600 "$DEPLOY_DIR/mosquitto/passwd"
else
  echo "WARNING: MQTT_USERNAME/MQTT_PASSWORD not set — mosquitto.prod.conf" >&2
  echo "  requires a passwd file to even start. Set both as GitHub Secrets." >&2
fi

# Authenticate with GHCR so docker compose pull can fetch private images.
mkdir -p ~/.docker
echo "${GHCR_TOKEN:-}" | docker login ghcr.io -u github --password-stdin

# Pull + recreate app services only — mosquitto is infrastructure and must
# NOT be restarted on code-only deploys (it holds live MQTT session state;
# restarting it drops every connected board mid-experiment). Watchtower is
# similarly left alone (it's already label-scoped to server+dashboard only —
# see docker-compose.prod.yml — so it can't touch other tenants on this
# shared host even if it did restart).
docker compose -f docker-compose.yml -f docker-compose.prod.yml pull --quiet server dashboard
docker compose -f docker-compose.yml -f docker-compose.prod.yml up -d --force-recreate --no-deps server dashboard
docker compose -f docker-compose.yml -f docker-compose.prod.yml up -d --remove-orphans

echo "== Hesperus deploy complete =="
