"""
Thesis IoT Server — Test Fixtures
"""

import os
os.environ["DATABASE_URL"] = "sqlite+aiosqlite://"

from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from fastapi.testclient import TestClient

# Force-import all modules so they exist before patching
import app.main  # noqa: F401
import app.api.routes  # noqa: F401
import app.api.scheduler_routes  # noqa: F401
import app.api.firmware_routes  # noqa: F401
import app.api.wifi_routes  # noqa: F401
import app.api.agent_routes  # noqa: F401
import app.api.benchmark_routes  # noqa: F401
import app.api.camera_routes  # noqa: F401
import app.mqtt.client  # noqa: F401
import app.db.database  # noqa: F401


@pytest.fixture(autouse=True)
def _owner_login_off(monkeypatch):
    """Owner login off unless a test turns it on (test_owner_auth.py), so an
    OWNER_PASSWORD in the environment can't 401 every other test."""
    from app.config import settings
    monkeypatch.setattr(settings, "owner_password", "")


@pytest.fixture
def client():
    """
    FastAPI test client with MQTT mocked out and in-memory DB.
    """
    mock_mqtt = MagicMock()
    mock_mqtt.connection = AsyncMock()
    mock_mqtt.client = MagicMock()
    mock_mqtt.client.disconnect = AsyncMock()
    mock_mqtt.publish = AsyncMock()

    # Create a fresh in-memory engine for each test
    from sqlalchemy.ext.asyncio import create_async_engine, async_sessionmaker, AsyncSession
    test_engine = create_async_engine(
        "sqlite+aiosqlite://",
        echo=False,
        connect_args={"check_same_thread": False},
    )
    test_session = async_sessionmaker(test_engine, class_=AsyncSession, expire_on_commit=False)

    with patch.object(app.main, "mqtt_client", mock_mqtt), \
         patch.object(app.api.routes, "mqtt_client", mock_mqtt), \
         patch.object(app.api.scheduler_routes, "mqtt_client", mock_mqtt), \
         patch.object(app.api.firmware_routes, "mqtt_client", mock_mqtt), \
         patch.object(app.api.wifi_routes, "mqtt_client", mock_mqtt), \
         patch.object(app.api.agent_routes, "mqtt_client", mock_mqtt), \
         patch.object(app.api.camera_routes, "mqtt_client", mock_mqtt), \
         patch.object(app.db.database, "engine", test_engine), \
         patch.object(app.db.database, "async_session", test_session):
        # Ensure settings has a valid Fernet key for WiFi config tests
        from cryptography.fernet import Fernet
        from app.config import settings
        original_key = settings.wifi_config_encryption_key
        settings.wifi_config_encryption_key = Fernet.generate_key().decode()

        from app.main import app as fastapi_app
        with TestClient(fastapi_app) as c:
            c._mock_mqtt = mock_mqtt
            yield c

        settings.wifi_config_encryption_key = original_key
