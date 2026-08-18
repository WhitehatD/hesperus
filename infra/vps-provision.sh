#!/usr/bin/env bash
# Hesperus one-time VPS provisioning — run ONCE as root on infra-core
# (46.62.200.8) to carve out an isolated slice for this project on a shared
# host that also runs Forgejo, ntfy, Umami, oauth2-proxy, TimescaleDB.
#
# This is the privileged bootstrap step that CANNOT itself be done by the
# restricted `hesperus-deploy` user (chicken-and-egg, same shape as the
# firmware OTA problem) — it creates that very user. Run it once, under
# explicit review, then every subsequent deploy goes through the restricted
# path (deploy.sh via SSH forced command) instead.
#
# Usage:
#   1. Locally: ssh-keygen -t ed25519 -f hesperus_deploy_key -N ""
#      (keep hesperus_deploy_key private — it becomes GitHub Secret
#      DEPLOY_SSH_KEY; hesperus_deploy_key.pub is the DEPLOY_PUBKEY input below)
#   2. scp this script to the VPS, or paste it in a root shell
#   3. DEPLOY_PUBKEY="ssh-ed25519 AAAA..." bash vps-provision.sh
#
# Idempotent where reasonably possible — safe to re-run, but not designed to
# be run automatically/repeatedly (this is a one-time setup script, not part
# of the regular deploy loop).

set -euo pipefail

DEPLOY_PUBKEY="${DEPLOY_PUBKEY:?Set DEPLOY_PUBKEY to the hesperus-deploy public key}"
REPO_URL="https://github.com/WhitehatD/hesperus.git"
DEPLOY_DIR="/opt/hesperus"

echo "== 1/6: /opt/hesperus + repo clone =="
mkdir -p "$DEPLOY_DIR"
if [ ! -d "$DEPLOY_DIR/.git" ]; then
  git clone "$REPO_URL" "$DEPLOY_DIR"
fi
chmod +x "$DEPLOY_DIR/infra/deploy.sh"

echo "== 2/6: hesperus-deploy restricted system user =="
if ! id -u hesperus-deploy >/dev/null 2>&1; then
  useradd --system --create-home --shell /usr/sbin/nologin hesperus-deploy
fi
usermod -aG docker hesperus-deploy   # required to run docker compose — NOTE this
                                      # is root-equivalent via the docker socket;
                                      # the ForceCommand restriction below is what
                                      # actually bounds the blast radius of the key
chown -R hesperus-deploy:hesperus-deploy "$DEPLOY_DIR"

install -d -m 700 -o hesperus-deploy -g hesperus-deploy /home/hesperus-deploy/.ssh
echo "$DEPLOY_PUBKEY" > /home/hesperus-deploy/.ssh/authorized_keys
chown hesperus-deploy:hesperus-deploy /home/hesperus-deploy/.ssh/authorized_keys
chmod 600 /home/hesperus-deploy/.ssh/authorized_keys

echo "== 3/6: sshd Match block — force deploy.sh, whitelist only the needed env vars =="
SSHD_DROPIN=/etc/ssh/sshd_config.d/hesperus-deploy.conf
cat > "$SSHD_DROPIN" <<'EOF'
# Hesperus restricted deploy user — added by infra/vps-provision.sh
Match User hesperus-deploy
    ForceCommand /opt/hesperus/deploy.sh
    AcceptEnv ANTHROPIC_API_KEY FIRMWARE_UPLOAD_TOKEN GEMINI_API_KEY GHCR_TOKEN
    PermitTTY no
    X11Forwarding no
    AllowTcpForwarding no
    AllowAgentForwarding no
EOF
sshd -t   # fail loudly before reloading if the config is broken
systemctl reload sshd

echo "== 4/6: ufw — board ingress ports only =="
ufw allow 8000/tcp comment 'hesperus-iot board ingress (HTTP upload)'
ufw allow 1883/tcp comment 'hesperus-iot board ingress (MQTT)'

echo "== 5/6: nginx vhost — hesperus.ciocandco.com =="
cat > /etc/nginx/sites-available/hesperus.ciocandco.com <<'EOF'
# Hesperus dashboard — reverse proxy to the loopback-bound Next.js container.
# Standalone LE cert, independent of the main ciocandco.com multi-SAN cert —
# a cert issue on either side can never affect the other.
upstream hesperus_dashboard { server 127.0.0.1:3002; }
upstream hesperus_server    { server 127.0.0.1:8000; }
upstream hesperus_mqtt_ws   { server 127.0.0.1:9001; }

server {
    listen 80;
    listen [::]:80;
    server_name hesperus.ciocandco.com;
    location /.well-known/acme-challenge/ { root /var/www/html; }
    location / { return 301 https://$host$request_uri; }
}

server {
    listen 443 ssl http2;
    listen [::]:443 ssl;
    server_name hesperus.ciocandco.com;

    ssl_certificate     /etc/letsencrypt/live/hesperus.ciocandco.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/hesperus.ciocandco.com/privkey.pem;
    include             /etc/letsencrypt/options-ssl-nginx.conf;
    ssl_dhparam         /etc/letsencrypt/ssl-dhparams.pem;

    add_header Strict-Transport-Security "max-age=31536000; includeSubDomains" always;
    add_header X-Content-Type-Options "nosniff" always;
    add_header X-Frame-Options "SAMEORIGIN" always;

    location = /healthz { access_log off; return 200 "ok\n"; }

    # MQTT-over-WebSocket — dashboard's browser client (useMQTT.ts falls back
    # to wss://<host>/mqtt when NEXT_PUBLIC_MQTT_WS_URL is unset at build time)
    location /mqtt {
        proxy_pass http://hesperus_mqtt_ws;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_read_timeout 3600s;
    }

    # /api/agent/* is served by the DASHBOARD itself (Next.js API routes:
    # agent/chat, agent/sessions, agent/sessions/[id]/messages) — NOT by
    # FastAPI. This block must come before the general /api/ block; nginx
    # prefix matching is longest-wins, so this takes precedence. Routing all
    # of /api/ to FastAPI would break the agent chat entirely.
    location /api/agent/ {
        proxy_pass http://hesperus_dashboard;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }

    # All other REST API paths are FastAPI (images, capture, firmware, energy,
    # schedules...). The dashboard's browser client falls back to same-origin
    # /api/* when NEXT_PUBLIC_API_URL is unset; no prefix stripping, URI as-is.
    location /api/ {
        proxy_pass http://hesperus_server;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }

    location / {
        proxy_pass http://hesperus_dashboard;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }
}
EOF
ln -sf /etc/nginx/sites-available/hesperus.ciocandco.com /etc/nginx/sites-enabled/hesperus.ciocandco.com

echo "== 6/6: certbot — standalone cert for hesperus.ciocandco.com =="
nginx -t && systemctl reload nginx   # reload with HTTP-only server block first (ACME challenge path needs to resolve)
certbot certonly --webroot -w /var/www/html -d hesperus.ciocandco.com --non-interactive --agree-tos -m alex@ciocandco.com
nginx -t && systemctl reload nginx   # reload again — now the HTTPS block's cert files exist

echo "== Done. Verify with the checks in the plan's Verification section. =="
