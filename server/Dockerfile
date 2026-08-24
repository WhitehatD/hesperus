FROM python:3.11-slim

WORKDIR /app

# Install deps first (layer caching)
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy application code
COPY app/ app/

# Create data directories
RUN mkdir -p /app/data/uploads

EXPOSE 8000

# No --reload here: this is the image CI builds and ships to prod
# (ghcr.io/whitehatd/hesperus-server:latest). --reload's file-watcher
# defaults to watching the whole CWD (/app), which includes /app/data —
# where docker-compose.prod.yml mounts the SQLite DB and uploads. Every
# capture/schedule/energy-telemetry write touched that volume and
# uvicorn silently killed + respawned the worker process (confirmed
# 2026-08-24: worker PID started 14 minutes after container start).
# Each respawn re-ran the FastAPI lifespan, opening a fresh MQTT
# connection while the previous one tore down — an MQTT publish
# straddling that window (e.g. dashboard/images/new right after an
# upload) was silently dropped: gmqtt's publish() doesn't raise on a
# torn-down connection, so the server-side try/except never caught it
# and nothing in the dashboard ever updated live. Local dev re-enables
# --reload via docker-compose.yml's `command:` override, correctly
# scoped to app/ only (not data/) — see the comment there.
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
