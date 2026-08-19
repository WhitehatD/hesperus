"""
Hesperus Server — Configuration
Environment-based settings using pydantic-settings.
"""

from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    """Application settings loaded from environment variables."""

    # ── App ─────────────────────────────────────────────
    app_name: str = "Hesperus — Autonomous IoT Visual Monitoring"
    debug: bool = True

    # ── MQTT ────────────────────────────────────────────
    mqtt_broker_host: str = "localhost"
    mqtt_broker_port: int = 1883
    mqtt_topic_commands: str = "device/stm32/commands"
    mqtt_topic_status: str = "device/stm32/status"
    mqtt_topic_dashboard_images: str = "dashboard/images/new"
    mqtt_topic_dashboard_analysis: str = "dashboard/analysis/new"
    mqtt_topic_dashboard_schedules: str = "dashboard/schedules/updated"

    # ── Database ────────────────────────────────────────
    database_url: str = "sqlite+aiosqlite:///./data/thesis.db"

    # ── AI Backends ─────────────────────────────────────
    # OpenRouter is the PRODUCTION default as of 2026-08-19 (one key,
    # OpenAI-compatible API, provider-agnostic model routing) — chat
    # planning/tool-use, schedule generation, and image analysis all prefer
    # it when openrouter_api_key is set. Hybrid routing: a cheap
    # tools-capable model for chat/schedule planning (never sees images),
    # a vision-capable model for image analysis — see planner/analysis
    # model settings below. Does NOT alter the frozen benchmark methodology
    # already published in results/findings_v3.md — those numbers were
    # measured against direct provider APIs and are historical; the
    # anthropic_api_key/gemini_api_key/vllm_* settings below stay in place
    # for scripts/run_benchmark.py and manual model_key selection. Only the
    # going-forward production system's default wiring changed.
    anthropic_api_key: str = ""
    claude_sonnet_model: str = "claude-sonnet-4-6"
    claude_haiku_model: str = "claude-haiku-4-5-20251001"

    openrouter_api_key: str = ""
    openrouter_base_url: str = "https://openrouter.ai/api/v1"
    # Used for the chat tool-use loop + schedule generation — text-only in
    # practice (this role never reasons about images), so the selection
    # criterion is cost + tool-calling reliability, not vision capability.
    # Cheapest tools-capable model on OpenRouter as of 2026-08-19 per live
    # pricing check (it happens to also support vision, unused here).
    openrouter_planner_model: str = "qwen/qwen3.7-flash"
    # Vision model for image analysis. Same model family already benchmarked
    # in results/findings_v3.md (self-hosted Qwen3-VL-30B-A3B) — known T1
    # quality tier — via OpenRouter instead of the VPN-gated Ernis tunnel.
    openrouter_analysis_model: str = "qwen/qwen3-vl-30b-a3b-instruct"

    # Alternate vision backends (thesis multi-backend benchmark)
    # Each open-weight model runs on its own llama-server instance — the
    # *_base_url settings let the engine pick the right backend by model_key.
    vllm_base_url: str = "http://localhost:8001/v1"          # qwen3-vl (30B)
    vllm_model: str = "Qwen/Qwen3-VL-30B-A3B"
    vllm_qwen25_base_url: str = "http://localhost:8002/v1"   # qwen2.5-vl (3B)
    vllm_qwen25_model: str = "Qwen/Qwen2.5-VL-3B-Instruct"
    gemini_api_key: str = ""
    gemini_model: str = "gemini-3-flash"

    # ── Storage ─────────────────────────────────────────
    upload_dir: str = "./data/uploads"
    firmware_dir: str = "./data/firmware"

    # ── Firmware OTA ───────────────────────────────────
    firmware_upload_token: str = ""  # API key for CI firmware uploads (empty = no auth)

    # ── MQTT Auth ───────────────────────────────────────
    # Empty = anonymous (dev only — mosquitto.conf allows it). Production uses
    # mosquitto.prod.conf (allow_anonymous false); these must be set to match
    # the password_file generated at deploy time (see infra/deploy.sh).
    mqtt_username: str = ""
    mqtt_password: str = ""

    # ── WiFi Config Encryption ────────────────────────
    # Fernet key for encrypting WiFi passwords at rest.
    # Generate with: python -c "from cryptography.fernet import Fernet; print(Fernet.generate_key().decode())"
    wifi_config_encryption_key: str = ""

    model_config = {"env_file": ".env", "env_file_encoding": "utf-8"}


settings = Settings()
