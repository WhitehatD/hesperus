#!/usr/bin/env bash
# Hesperus deploy script — runs ON infra-core (46.62.200.8) as the restricted
# `hesperus-deploy` system user, invoked via an SSH forced command (sshd_config
# Match block: ForceCommand /opt/hesperus/infra/deploy.sh — see
# vps-provision.sh; not an authorized_keys command= prefix).
#
# Because it's a forced command, whatever "script" text the CI client sends is
# IGNORED — this script always runs instead, regardless of what ci.yml's
# `script:` field contains.
#
# SECRETS DO NOT FLOW THROUGH THIS PATH. Originally this script sourced
# secrets from SSH env vars sent by ci.yml's `envs:`/`env:` (relying on
# SSH's AcceptEnv/SendEnv). Empirically, appleboy/ssh-action's env passing
# does not survive ForceCommand — verified 2026-08-18 on the actual first
# deploy: MQTT_USERNAME/MQTT_PASSWORD arrived empty despite being set as
# GitHub Secrets and listed in both `envs:` and the sshd AcceptEnv allowlist.
# Root-caused to appleboy/ssh-action injecting envs into the CLIENT'S
# requested command string (shell `export` prefix) rather than genuine SSH
# protocol env-forwarding — and ForceCommand discards the client's entire
# requested command, exports and all.
#
# Redesigned instead: /opt/hesperus/.env.prod is a PERSISTENT file, written
# once by root (see infra/rotate-secrets.sh) whenever a secret actually
# changes — not regenerated on every deploy. This is arguably better anyway:
# the CI deploy key now carries zero secrets, so even a fully leaked
# DEPLOY_SSH_KEY can't exfiltrate anything beyond "redeploy the current
# images" — a strictly smaller blast radius than the original design.
#
# GHCR login was also removed: ghcr.io/whitehatd/hesperus-{server,dashboard}
# are public packages, no auth needed to pull.
#
# Deliberately hard-scoped to /opt/hesperus — no `find` across the
# filesystem, no clone-fallback, no touching anything outside this directory.
# This is the guest-tenant-owns-its-own-infra-as-code design: infra-core is a
# shared box, this script must never be able to reach
# Forgejo/ntfy/Umami/oauth2-proxy/etc.

set -euo pipefail

DEPLOY_DIR="/opt/hesperus"
cd "$DEPLOY_DIR"

echo "== Hesperus deploy: $(date -u +%FT%TZ) =="

git fetch origin main
git reset --hard origin/main

ENV_FILE="$DEPLOY_DIR/.env.prod"
if [ ! -f "$ENV_FILE" ]; then
  echo "ERROR: $ENV_FILE does not exist. Run infra/rotate-secrets.sh as root" >&2
  echo "  first (one-time secret provisioning, outside the CI hot path)." >&2
  exit 1
fi
if [ ! -f "$DEPLOY_DIR/mosquitto/passwd" ]; then
  echo "ERROR: $DEPLOY_DIR/mosquitto/passwd does not exist. Run" >&2
  echo "  infra/rotate-secrets.sh as root first." >&2
  exit 1
fi

# docker compose's ${VAR} substitution (docker-compose.prod.yml) reads from
# this process's environment — source the persistent env file into it.
set -a
# shellcheck disable=SC1090
source "$ENV_FILE"
set +a

# Pull + recreate app services only — mosquitto is infrastructure and must
# NOT be restarted on code-only deploys (it holds live MQTT session state;
# restarting it drops every connected board mid-experiment). Watchtower is
# similarly left alone (it's already label-scoped to server+dashboard only —
# see docker-compose.prod.yml — so it can't touch other tenants on this
# shared host even if it did restart).
docker compose -f docker-compose.yml -f docker-compose.prod.yml pull --quiet server dashboard
docker compose -f docker-compose.yml -f docker-compose.prod.yml up -d --force-recreate --no-deps server dashboard
docker compose -f docker-compose.yml -f docker-compose.prod.yml up -d --remove-orphans

# mosquitto itself is never restarted (see above — it holds live MQTT
# session state), but that means an mosquitto/acl or mosquitto/passwd change
# on disk was previously NEVER applied by this pipeline at all: the broker
# only reads those files at process start. Discovered 2026-08-24 when an ACL
# fix (granting hesperus-board write on dashboard/#) sat on disk, correctly
# bind-mounted, silently inert, because nothing ever told the running broker
# to re-read it.
#
# Fix: mosquitto reloads its password file and ACL file on SIGHUP WITHOUT
# dropping existing client connections (documented mosquitto behavior —
# SIGHUP is a config reload, not a restart; only listener-level changes need
# a full restart). Send it unconditionally on every deploy rather than
# gating on "did acl/passwd change this run" — a git-diff-based gate can't
# reliably tell whether the CURRENTLY RUNNING deploy.sh process is the old
# or new version of itself (a script mid-execution keeps running from its
# already-buffered content even after `git reset --hard` rewrites the file
# on disk), so a conditional check could silently skip the one deploy that
# actually needed it. Unconditional SIGHUP has no such failure mode: safe
# and idempotent every time, whether or not anything changed. If mosquitto
# isn't up yet (first-ever deploy) this fails harmlessly and is ignored,
# since `docker compose up -d --remove-orphans` above already started it
# fresh with current files in that case.
echo "== Reloading mosquitto config (SIGHUP, no restart, no dropped clients) =="
docker compose -f docker-compose.yml -f docker-compose.prod.yml kill -s HUP mosquitto || true

echo "== Hesperus deploy complete =="
