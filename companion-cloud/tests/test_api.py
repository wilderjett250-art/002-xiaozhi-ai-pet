import importlib
import io
import sqlite3
import struct
from types import SimpleNamespace

from fastapi.testclient import TestClient


def build_client(tmp_path, monkeypatch):
    monkeypatch.setenv("PET_CLOUD_DATA_DIR", str(tmp_path / "data"))
    monkeypatch.setenv("PET_CLOUD_ADMIN_TOKEN", "admin-test-token-0123456789")
    monkeypatch.setenv("PET_CLOUD_DEVICE_TOKEN", "device-test-token-0123456789")
    import app
    importlib.reload(app)
    return TestClient(app.app), app


def device_headers(device_id="device-01"):
    return {"Authorization": "Bearer device-test-token-0123456789", "X-Device-Id": device_id}


def admin_headers():
    return {"X-Admin-Token": "admin-test-token-0123456789"}


def test_dashboard_uses_subpath_relative_assets_and_api_urls(tmp_path, monkeypatch):
    client, _ = build_client(tmp_path, monkeypatch)
    with client:
        response = client.get("/")
    assert response.status_code == 200
    html = response.text
    assert 'href="favicon.png"' in html
    assert 'src="vendor/lucide.min.js"' in html
    assert "fetch(relativeUrl(path)" in html
    assert "fetch(relativeUrl(`/api/v1/admin/photos/" in html
    assert "/api/v1/admin/devices/${encodeURIComponent(deviceId)}/greet" in html
    assert 'class="secondary-button greet"' in html
    assert 'data-view="ambient"' in html
    assert 'id="ambient-form"' in html
    assert 'id="ambient-debug-audio"' in html
    assert 'class="secondary-button play-ambient"' in html
    assert "/api/v1/admin/ambient-events/${eventId}/audio" in html
    assert 'href="/favicon.png"' not in html
    assert 'src="/vendor/lucide.min.js"' not in html


def test_v1_database_migrates_dual_network_fields(tmp_path, monkeypatch):
    data_dir = tmp_path / "data"
    data_dir.mkdir()
    database = data_dir / "companion-cloud.db"
    with sqlite3.connect(database) as db:
        db.execute(
            """
            CREATE TABLE devices (
                device_id TEXT PRIMARY KEY, name TEXT NOT NULL,
                firmware TEXT NOT NULL DEFAULT '', network TEXT NOT NULL DEFAULT '',
                state TEXT NOT NULL DEFAULT '', last_seen_ms INTEGER NOT NULL,
                created_at_ms INTEGER NOT NULL
            )
            """
        )
        db.execute(
            "INSERT INTO devices VALUES (?, ?, ?, ?, ?, ?, ?)",
            ("legacy-device", "旧设备", "2.2.6", "wifi", "idle", 1, 1),
        )

    monkeypatch.setenv("PET_CLOUD_DATA_DIR", str(data_dir))
    monkeypatch.setenv("PET_CLOUD_ADMIN_TOKEN", "admin-test-token-0123456789")
    monkeypatch.setenv("PET_CLOUD_DEVICE_TOKEN", "device-test-token-0123456789")
    import app
    importlib.reload(app)
    app.initialize_database()

    with app.connect_db() as db:
        columns = {row["name"] for row in db.execute("PRAGMA table_info(devices)")}
        legacy = db.execute("SELECT * FROM devices WHERE device_id = 'legacy-device'").fetchone()
    assert {
        "network_primary", "network_standby", "automatic_failover", "network_connected", "ambient_enabled"
    } <= columns
    assert legacy["name"] == "旧设备"
    assert legacy["network"] == "wifi"


def test_v130_database_migrates_ambient_debug_fields(tmp_path, monkeypatch):
    data_dir = tmp_path / "data"
    data_dir.mkdir()
    database = data_dir / "companion-cloud.db"
    with sqlite3.connect(database) as db:
        db.execute(
            "CREATE TABLE ambient_settings (device_id TEXT PRIMARY KEY, enabled INTEGER NOT NULL DEFAULT 1, "
            "cooldown_seconds INTEGER NOT NULL DEFAULT 90, max_per_hour INTEGER NOT NULL DEFAULT 3, "
            "quiet_start TEXT NOT NULL DEFAULT '23:00', quiet_end TEXT NOT NULL DEFAULT '07:00', "
            "updated_at_ms INTEGER NOT NULL)"
        )
        db.execute(
            "CREATE TABLE ambient_events (id INTEGER PRIMARY KEY AUTOINCREMENT, device_id TEXT NOT NULL, "
            "duration_ms INTEGER NOT NULL, byte_size INTEGER NOT NULL, status TEXT NOT NULL, "
            "transcript TEXT NOT NULL DEFAULT '', action TEXT NOT NULL DEFAULT '', "
            "response_text TEXT NOT NULL DEFAULT '', reason TEXT NOT NULL DEFAULT '', "
            "decision_engine TEXT NOT NULL DEFAULT '', created_at_ms INTEGER NOT NULL, processed_at_ms INTEGER)"
        )

    monkeypatch.setenv("PET_CLOUD_DATA_DIR", str(data_dir))
    monkeypatch.setenv("PET_CLOUD_ADMIN_TOKEN", "admin-test-token-0123456789")
    monkeypatch.setenv("PET_CLOUD_DEVICE_TOKEN", "device-test-token-0123456789")
    import app
    importlib.reload(app)
    app.initialize_database()

    with app.connect_db() as db:
        settings_columns = {row["name"] for row in db.execute("PRAGMA table_info(ambient_settings)")}
        event_columns = {row["name"] for row in db.execute("PRAGMA table_info(ambient_events)")}
    assert {"debug_audio_enabled", "debug_retention_days", "debug_max_mb"} <= settings_columns
    assert {
        "debug_recorded", "debug_raw_file", "debug_wav_file", "peak_dbfs", "rms_dbfs",
        "decode_ms", "asr_ms", "decision_ms", "tts_ms", "pipeline_ms",
    } <= event_columns


def test_chat_photo_and_remote_capture_flow(tmp_path, monkeypatch):
    client, _ = build_client(tmp_path, monkeypatch)
    with client:
        heartbeat = client.post(
            "/api/v1/device/heartbeat",
            headers=device_headers(),
            json={
                "name": "桌宠一号",
                "firmware": "2.2.11",
                "network": "4g",
                "network_primary": "4g",
                "network_standby": "wifi",
                "automatic_failover": True,
                "network_connected": True,
                "state": "idle",
            },
        )
        assert heartbeat.status_code == 200

        chat = client.post(
            "/api/v1/device/chats",
            headers=device_headers(),
            json={"session_id": "session-1", "role": "user", "content": "拍一张照片"},
        )
        assert chat.status_code == 201

        command = client.post(
            "/api/v1/admin/devices/device-01/capture",
            headers=admin_headers(),
        )
        assert command.status_code == 202
        command_id = command.json()["command_id"]

        next_command = client.get(
            "/api/v1/device/commands/next?wait_seconds=0",
            headers=device_headers(),
        )
        assert next_command.json()["command"]["id"] == command_id

        photo = client.post(
            "/api/v1/device/photos",
            headers=device_headers(),
            data={"source": "admin", "command_id": str(command_id), "width": "640", "height": "480"},
            files={"image": ("capture.jpg", io.BytesIO(b"\xff\xd8test-jpeg\xff\xd9"), "image/jpeg")},
        )
        assert photo.status_code == 201
        photo_id = photo.json()["photo_id"]
        image = client.get(f"/api/v1/admin/photos/{photo_id}/image", headers=admin_headers())
        assert image.status_code == 200
        assert image.content.startswith(b"\xff\xd8")

        devices = client.get("/api/v1/admin/devices", headers=admin_headers()).json()["devices"]
        assert devices[0]["online"] is True
        assert devices[0]["network"] == "4g"
        assert devices[0]["network_primary"] == "4g"
        assert devices[0]["network_standby"] == "wifi"
        assert devices[0]["automatic_failover"] == 1
        assert devices[0]["network_connected"] == 1
        assert devices[0]["chat_count"] == 1
        assert devices[0]["photo_count"] == 1


def test_auto_photo_source_is_rejected(tmp_path, monkeypatch):
    client, _ = build_client(tmp_path, monkeypatch)
    with client:
        client.post("/api/v1/device/heartbeat", headers=device_headers(), json={})
        response = client.post(
            "/api/v1/device/photos",
            headers=device_headers(),
            data={"source": "auto"},
            files={"image": ("capture.jpg", io.BytesIO(b"image"), "image/jpeg")},
        )
        assert response.status_code == 422


def test_running_command_is_requeued_after_delivery_lease(tmp_path, monkeypatch):
    client, app_module = build_client(tmp_path, monkeypatch)
    clock = {"value": 1_000_000}
    monkeypatch.setattr(app_module, "now_ms", lambda: clock["value"])

    with client:
        client.post("/api/v1/device/heartbeat", headers=device_headers(), json={})
        queued = client.post(
            "/api/v1/admin/devices/device-01/capture",
            headers=admin_headers(),
        )
        command_id = queued.json()["command_id"]

        first_delivery = client.get(
            "/api/v1/device/commands/next?wait_seconds=0",
            headers=device_headers(),
        )
        assert first_delivery.json()["command"]["id"] == command_id

        clock["value"] += app_module.COMMAND_LEASE_MS + 1
        redelivery = client.get(
            "/api/v1/device/commands/next?wait_seconds=0",
            headers=device_headers(),
        )
        assert redelivery.json()["command"]["id"] == command_id


def test_remote_greeting_command_flow(tmp_path, monkeypatch):
    client, _ = build_client(tmp_path, monkeypatch)
    with client:
        client.post("/api/v1/device/heartbeat", headers=device_headers(), json={})
        queued = client.post(
            "/api/v1/admin/devices/device-01/greet",
            headers=admin_headers(),
        )
        assert queued.status_code == 202
        assert queued.json()["message"] == "你在干嘛呀？"

        delivered = client.get(
            "/api/v1/device/commands/next?wait_seconds=0",
            headers=device_headers(),
        ).json()["command"]
        assert delivered["command"] == "greet"
        assert delivered["payload"] == {"message": "你在干嘛呀？"}

        completed = client.post(
            f"/api/v1/device/commands/{delivered['id']}/complete",
            headers=device_headers(),
            json={"status": "completed"},
        )
        assert completed.status_code == 200


def test_ambient_settings_and_short_speech_upload(tmp_path, monkeypatch):
    client, app_module = build_client(tmp_path, monkeypatch)
    processed = []
    monkeypatch.setattr(
        app_module.speech_engine,
        "status",
        lambda: {"ready": True, "asr_ready": True, "tts_ready": True, "engine": "test", "provider": "cpu"},
    )
    monkeypatch.setattr(
        app_module,
        "process_ambient_event",
        lambda event_id, device_id, content, origin: processed.append((event_id, device_id, content, origin)),
    )

    packets = [b"\x01"] * 14
    content = struct.pack("<4sHHH", b"CAP1", 16000, 60, len(packets))
    content += b"".join(struct.pack("<H", len(packet)) + packet for packet in packets)

    with client:
        heartbeat = client.post("/api/v1/device/heartbeat", headers=device_headers(), json={})
        assert heartbeat.status_code == 200
        assert heartbeat.json()["ambient"] == {
            "supported": True,
            "enabled": True,
            "speech_ready": True,
            "cooldown_seconds": 90,
            "max_per_hour": 3,
        }

        settings = client.post(
            "/api/v1/admin/devices/device-01/ambient",
            headers=admin_headers(),
            json={
                "enabled": True,
                "cooldown_seconds": 120,
                "max_per_hour": 4,
                "quiet_start": "22:30",
                "quiet_end": "07:30",
                "debug_audio_enabled": True,
                "debug_retention_days": 3,
                "debug_max_mb": 200,
            },
        )
        assert settings.status_code == 200
        delivered = client.get(
            "/api/v1/device/commands/next?wait_seconds=0", headers=device_headers()
        ).json()["command"]
        assert delivered["command"] == "ambient_config"
        assert delivered["payload"] == {"enabled": True}

        upload = client.post(
            "/api/v1/device/ambient-audio",
            headers={**device_headers(), "Content-Type": "application/x-cat-opus"},
            content=content,
        )
        assert upload.status_code == 202
        assert upload.json()["duration_ms"] == 840
        assert upload.json()["debug_recorded"] is True
        assert processed and processed[0][1] == "device-01"

        events = client.get("/api/v1/admin/ambient-events", headers=admin_headers()).json()["events"]
        assert events[0]["status"] == "pending"
        assert events[0]["debug_recorded"] is True
        assert events[0]["has_debug_raw"] is True
        assert events[0]["has_debug_wav"] is False
        assert events[0]["sample_rate"] == 16000
        assert events[0]["frame_duration_ms"] == 60
        assert events[0]["frame_count"] == 14

        event_id = events[0]["id"]
        denied = client.get(f"/api/v1/admin/ambient-events/{event_id}/audio?kind=raw")
        assert denied.status_code == 403
        raw = client.get(
            f"/api/v1/admin/ambient-events/{event_id}/audio?kind=raw", headers=admin_headers()
        )
        assert raw.status_code == 200
        assert raw.content == content

        cleared = client.delete(
            "/api/v1/admin/devices/device-01/ambient-debug-audio", headers=admin_headers()
        )
        assert cleared.status_code == 200
        assert cleared.json()["cleared_events"] == 1
        retained = client.get("/api/v1/admin/ambient-events", headers=admin_headers()).json()["events"]
        assert retained[0]["has_debug_raw"] is False
        assert retained[0]["status"] == "pending"


def test_ambient_debug_audio_is_not_saved_by_default(tmp_path, monkeypatch):
    client, app_module = build_client(tmp_path, monkeypatch)
    monkeypatch.setattr(
        app_module.speech_engine,
        "status",
        lambda: {"ready": True, "asr_ready": True, "tts_ready": True, "engine": "test", "provider": "cpu"},
    )
    monkeypatch.setattr(app_module, "process_ambient_event", lambda *args: None)
    packets = [b"\x01"] * 14
    content = struct.pack("<4sHHH", b"CAP1", 16000, 60, len(packets))
    content += b"".join(struct.pack("<H", len(packet)) + packet for packet in packets)

    with client:
        client.post("/api/v1/device/heartbeat", headers=device_headers(), json={})
        upload = client.post(
            "/api/v1/device/ambient-audio",
            headers={**device_headers(), "Content-Type": "application/x-cat-opus"},
            content=content,
        )
        assert upload.status_code == 202
        assert upload.json()["debug_recorded"] is False
        event = client.get("/api/v1/admin/ambient-events", headers=admin_headers()).json()["events"][0]
        assert event["debug_recorded"] is False
        assert event["has_debug_raw"] is False
        assert event["has_debug_wav"] is False
        assert list(app_module.AMBIENT_DEBUG_DIR.iterdir()) == []


def test_ambient_debug_pipeline_saves_playable_wav_and_metrics(tmp_path, monkeypatch):
    client, app_module = build_client(tmp_path, monkeypatch)
    monkeypatch.setattr(
        app_module.speech_engine,
        "status",
        lambda: {"ready": True, "asr_ready": True, "tts_ready": True, "engine": "test", "provider": "cpu"},
    )
    monkeypatch.setattr(app_module.speech_engine, "transcribe", lambda pcm, sample_rate: "这是调试语音")
    monkeypatch.setattr(
        app_module.decision_engine,
        "decide",
        lambda transcript, recent: SimpleNamespace(
            action="ignore", response="", reason="测试规则忽略", engine="test-rules"
        ),
    )
    pcm = (struct.pack("<h", 12000) + struct.pack("<h", -12000)) * 6720
    monkeypatch.setattr(app_module, "decode_cat_opus", lambda content: (pcm, 16000, 840))
    packets = [b"\x01"] * 14
    content = struct.pack("<4sHHH", b"CAP1", 16000, 60, len(packets))
    content += b"".join(struct.pack("<H", len(packet)) + packet for packet in packets)

    with client:
        client.post(
            "/api/v1/device/heartbeat",
            headers=device_headers(),
            json={"firmware": "2.3.0", "network": "4g", "state": "idle"},
        )
        configured = client.post(
            "/api/v1/admin/devices/device-01/ambient",
            headers=admin_headers(),
            json={
                "enabled": True,
                "cooldown_seconds": 90,
                "max_per_hour": 3,
                "quiet_start": "00:00",
                "quiet_end": "00:00",
                "debug_audio_enabled": True,
                "debug_retention_days": 3,
                "debug_max_mb": 200,
            },
        )
        assert configured.status_code == 200
        upload = client.post(
            "/api/v1/device/ambient-audio",
            headers={**device_headers(), "Content-Type": "application/x-cat-opus"},
            content=content,
        )
        assert upload.status_code == 202

        event = client.get("/api/v1/admin/ambient-events", headers=admin_headers()).json()["events"][0]
        assert event["status"] == "ignored"
        assert event["transcript"] == "这是调试语音"
        assert event["reason"] == "测试规则忽略"
        assert event["has_debug_raw"] is True
        assert event["has_debug_wav"] is True
        assert event["peak_dbfs"] < 0
        assert event["rms_dbfs"] < 0
        assert event["firmware_snapshot"] == "2.3.0"
        assert event["network_snapshot"] == "4g"

        wav = client.get(
            f"/api/v1/admin/ambient-events/{event['id']}/audio?kind=wav", headers=admin_headers()
        )
        assert wav.status_code == 200
        assert wav.content.startswith(b"RIFF")
        deleted = client.delete(
            f"/api/v1/admin/ambient-events/{event['id']}", headers=admin_headers()
        )
        assert deleted.status_code == 200
        assert list(app_module.AMBIENT_DEBUG_DIR.iterdir()) == []


def test_device_clock_skew_is_normalized_for_chats_and_photos(tmp_path, monkeypatch):
    client, app_module = build_client(tmp_path, monkeypatch)
    server_now = 1_800_000_000_000
    monkeypatch.setattr(app_module, "now_ms", lambda: server_now)
    device_time = server_now + 8 * 60 * 60 * 1000

    with client:
        client.post("/api/v1/device/heartbeat", headers=device_headers(), json={})
        chat = client.post(
            "/api/v1/device/chats",
            headers=device_headers(),
            json={
                "session_id": "clock-skew",
                "role": "user",
                "content": "现在几点",
                "occurred_at_ms": device_time,
            },
        )
        assert chat.status_code == 201

        photo = client.post(
            "/api/v1/device/photos",
            headers=device_headers(),
            data={"source": "voice", "captured_at_ms": str(device_time)},
            files={"image": ("capture.jpg", io.BytesIO(b"\xff\xd8clock\xff\xd9"), "image/jpeg")},
        )
        assert photo.status_code == 201

        chats = client.get("/api/v1/admin/chats", headers=admin_headers()).json()["chats"]
        photos = client.get("/api/v1/admin/photos", headers=admin_headers()).json()["photos"]
        assert chats[0]["occurred_at_ms"] == server_now
        assert photos[0]["captured_at_ms"] == server_now


def test_photo_filename_is_sanitized_and_admin_requires_active_command(tmp_path, monkeypatch):
    client, app_module = build_client(tmp_path, monkeypatch)
    unsafe_device_id = "../camera/../../pet-01"
    with client:
        voice_photo = client.post(
            "/api/v1/device/photos",
            headers=device_headers(unsafe_device_id),
            data={"source": "voice"},
            files={"image": ("capture.jpg", io.BytesIO(b"\xff\xd8safe\xff\xd9"), "image/jpeg")},
        )
        assert voice_photo.status_code == 201
        photo_id = voice_photo.json()["photo_id"]
        with app_module.connect_db() as db:
            file_name = db.execute("SELECT file_name FROM photos WHERE id = ?", (photo_id,)).fetchone()[0]
        assert "/" not in file_name
        assert "\\" not in file_name
        assert (app_module.PHOTO_DIR / file_name).resolve().parent == app_module.PHOTO_DIR

        invalid_admin_photo = client.post(
            "/api/v1/device/photos",
            headers=device_headers(),
            data={"source": "admin", "command_id": "999999"},
            files={"image": ("capture.jpg", io.BytesIO(b"\xff\xd8safe\xff\xd9"), "image/jpeg")},
        )
        assert invalid_admin_photo.status_code == 409


def test_private_gateway_and_remote_ota_flow(tmp_path, monkeypatch):
    client, _ = build_client(tmp_path, monkeypatch)
    firmware = b"\xe9" + b"cat-pet-firmware" * 128
    with client:
        heartbeat = client.post(
            "/api/v1/device/heartbeat",
            headers=device_headers(),
            json={"name": "Cat Pet", "firmware": "2.0.3", "network": "wifi", "state": "idle"},
        )
        assert heartbeat.status_code == 200

        gateway = client.post(
            "/api/v1/device/gateway",
            headers=device_headers(),
            json={"action": "status", "request": "", "parameters": "{}"},
        )
        assert gateway.status_code == 200
        assert gateway.json()["device"]["firmware"] == "2.0.3"

        upload = client.post(
            "/api/v1/admin/firmware",
            headers=admin_headers(),
            data={"version": "2.1.0-cat"},
            files={"firmware": ("xiaozhi.bin", io.BytesIO(firmware), "application/octet-stream")},
        )
        assert upload.status_code == 201
        release = upload.json()
        assert len(release["sha256"]) == 64

        queued = client.post(
            "/api/v1/admin/devices/device-01/ota",
            headers=admin_headers(),
            json={"release_id": release["release_id"]},
        )
        assert queued.status_code == 202

        command = client.get(
            "/api/v1/device/commands/next?wait_seconds=0",
            headers=device_headers(),
        ).json()["command"]
        assert command["command"] == "firmware_update"
        assert command["payload"]["version"] == "2.1.0-cat"
        assert command["payload"]["sha256"] == release["sha256"]

        download = client.get(command["payload"]["url"])
        assert download.status_code == 200
        assert download.content == firmware

        completed = client.post(
            f"/api/v1/device/commands/{command['id']}/complete",
            headers=device_headers(),
            json={"status": "completed"},
        )
        assert completed.status_code == 200
