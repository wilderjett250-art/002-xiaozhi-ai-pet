from __future__ import annotations

import asyncio
import hashlib
import json
import os
import re
import secrets
import sqlite3
import threading
import time
import uuid
from contextlib import asynccontextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Annotated, Literal
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from fastapi import BackgroundTasks, Depends, FastAPI, File, Form, Header, HTTPException, Query, Request, UploadFile
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from ambient_audio import AmbientAudioError, decode_cat_opus, inspect_cat_opus, pcm_diagnostics, write_pcm_wav
from ambient_decision import decision_engine
from speech_engine import speech_engine


BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = Path(os.getenv("PET_CLOUD_DATA_DIR", BASE_DIR / "data")).resolve()
PHOTO_DIR = DATA_DIR / "photos"
FIRMWARE_DIR = DATA_DIR / "firmware"
SPEECH_DIR = DATA_DIR / "speech"
AMBIENT_DEBUG_DIR = DATA_DIR / "ambient-debug"
DB_PATH = DATA_DIR / "companion-cloud.db"
ROOT_PATH = os.getenv("PET_CLOUD_ROOT_PATH", "").strip().rstrip("/")
if ROOT_PATH and not ROOT_PATH.startswith("/"):
    ROOT_PATH = "/" + ROOT_PATH


def required_token(name: str) -> str:
    value = os.getenv(name, "").strip()
    if len(value) < 24:
        raise RuntimeError(f"{name} must contain at least 24 characters")
    return value


ADMIN_TOKEN = required_token("PET_CLOUD_ADMIN_TOKEN")
DEVICE_TOKEN = required_token("PET_CLOUD_DEVICE_TOKEN")
ONLINE_WINDOW_SECONDS = 45
COMMAND_LEASE_MS = 90 * 1000
MAX_DEVICE_CLOCK_SKEW_MS = 5 * 60 * 1000
MAX_PHOTO_BYTES = 2 * 1024 * 1024
MAX_FIRMWARE_BYTES = 5 * 1024 * 1024
MAX_AMBIENT_AUDIO_BYTES = 128 * 1024
MIN_AMBIENT_DURATION_MS = 800
MAX_AMBIENT_DURATION_MS = 12000
SPEECH_ASSET_TTL_MS = 7 * 24 * 60 * 60 * 1000
DEFAULT_AMBIENT_ENABLED = os.getenv("PET_CLOUD_AMBIENT_DEFAULT_ENABLED", "true").lower() in {"1", "true", "yes", "on"}
DEFAULT_AMBIENT_DEBUG_AUDIO = os.getenv("PET_CLOUD_AMBIENT_DEBUG_AUDIO", "false").lower() in {"1", "true", "yes", "on"}
DEFAULT_AMBIENT_DEBUG_RETENTION_DAYS = max(1, min(30, int(os.getenv("PET_CLOUD_AMBIENT_DEBUG_RETENTION_DAYS", "3"))))
DEFAULT_AMBIENT_DEBUG_MAX_MB = max(20, min(2048, int(os.getenv("PET_CLOUD_AMBIENT_DEBUG_MAX_MB", "200"))))
AMBIENT_PROCESS_LOCK = threading.Lock()
try:
    LOCAL_TIMEZONE = ZoneInfo(os.getenv("PET_CLOUD_TIMEZONE", "Asia/Shanghai"))
except ZoneInfoNotFoundError:
    LOCAL_TIMEZONE = timezone(timedelta(hours=8), name="UTC+08:00")


def now_ms() -> int:
    return int(time.time() * 1000)


def normalize_device_timestamp(value: int, received_at_ms: int) -> int:
    if value <= 0 or abs(value - received_at_ms) > MAX_DEVICE_CLOCK_SKEW_MS:
        return received_at_ms
    return value


def safe_file_stem(value: str) -> str:
    stem = "".join(char if char.isascii() and (char.isalnum() or char in "-_") else "-" for char in value)
    return stem.strip("-")[:64] or "device"


def connect_db() -> sqlite3.Connection:
    connection = sqlite3.connect(DB_PATH, timeout=15)
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA journal_mode=WAL")
    connection.execute("PRAGMA foreign_keys=ON")
    return connection


def initialize_database() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    PHOTO_DIR.mkdir(parents=True, exist_ok=True)
    FIRMWARE_DIR.mkdir(parents=True, exist_ok=True)
    SPEECH_DIR.mkdir(parents=True, exist_ok=True)
    AMBIENT_DEBUG_DIR.mkdir(parents=True, exist_ok=True)
    with connect_db() as db:
        db.executescript(
            """
            CREATE TABLE IF NOT EXISTS devices (
                device_id TEXT PRIMARY KEY,
                name TEXT NOT NULL,
                firmware TEXT NOT NULL DEFAULT '',
                network TEXT NOT NULL DEFAULT '',
                network_primary TEXT NOT NULL DEFAULT '',
                network_standby TEXT NOT NULL DEFAULT '',
                automatic_failover INTEGER NOT NULL DEFAULT 0,
                network_connected INTEGER NOT NULL DEFAULT 0,
                ambient_enabled INTEGER NOT NULL DEFAULT 0,
                state TEXT NOT NULL DEFAULT '',
                last_seen_ms INTEGER NOT NULL,
                created_at_ms INTEGER NOT NULL
            );

            CREATE TABLE IF NOT EXISTS chats (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                session_id TEXT NOT NULL DEFAULT '',
                role TEXT NOT NULL CHECK(role IN ('user', 'assistant')),
                content TEXT NOT NULL,
                occurred_at_ms INTEGER NOT NULL,
                created_at_ms INTEGER NOT NULL,
                FOREIGN KEY(device_id) REFERENCES devices(device_id) ON DELETE CASCADE
            );
            CREATE INDEX IF NOT EXISTS idx_chats_device_time
                ON chats(device_id, occurred_at_ms DESC);

            CREATE TABLE IF NOT EXISTS photos (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                source TEXT NOT NULL CHECK(source IN ('voice', 'admin')),
                file_name TEXT NOT NULL UNIQUE,
                content_type TEXT NOT NULL,
                byte_size INTEGER NOT NULL,
                width INTEGER NOT NULL DEFAULT 0,
                height INTEGER NOT NULL DEFAULT 0,
                prompt TEXT NOT NULL DEFAULT '',
                analysis TEXT NOT NULL DEFAULT '',
                command_id INTEGER,
                captured_at_ms INTEGER NOT NULL,
                created_at_ms INTEGER NOT NULL,
                FOREIGN KEY(device_id) REFERENCES devices(device_id) ON DELETE CASCADE
            );
            CREATE INDEX IF NOT EXISTS idx_photos_device_time
                ON photos(device_id, captured_at_ms DESC);

            CREATE TABLE IF NOT EXISTS commands (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                command TEXT NOT NULL,
                payload_json TEXT NOT NULL DEFAULT '{}',
                status TEXT NOT NULL CHECK(status IN ('queued', 'running', 'completed', 'failed')),
                requested_at_ms INTEGER NOT NULL,
                started_at_ms INTEGER,
                completed_at_ms INTEGER,
                photo_id INTEGER,
                error TEXT NOT NULL DEFAULT '',
                FOREIGN KEY(device_id) REFERENCES devices(device_id) ON DELETE CASCADE,
                FOREIGN KEY(photo_id) REFERENCES photos(id) ON DELETE SET NULL
            );
            CREATE INDEX IF NOT EXISTS idx_commands_device_status
                ON commands(device_id, status, requested_at_ms);

            CREATE TABLE IF NOT EXISTS firmware_releases (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                version TEXT NOT NULL,
                file_name TEXT NOT NULL UNIQUE,
                byte_size INTEGER NOT NULL,
                sha256 TEXT NOT NULL,
                download_token TEXT NOT NULL UNIQUE,
                created_at_ms INTEGER NOT NULL
            );

            CREATE TABLE IF NOT EXISTS ambient_settings (
                device_id TEXT PRIMARY KEY,
                enabled INTEGER NOT NULL DEFAULT 1,
                cooldown_seconds INTEGER NOT NULL DEFAULT 90,
                max_per_hour INTEGER NOT NULL DEFAULT 3,
                quiet_start TEXT NOT NULL DEFAULT '23:00',
                quiet_end TEXT NOT NULL DEFAULT '07:00',
                debug_audio_enabled INTEGER NOT NULL DEFAULT 0,
                debug_retention_days INTEGER NOT NULL DEFAULT 3,
                debug_max_mb INTEGER NOT NULL DEFAULT 200,
                updated_at_ms INTEGER NOT NULL,
                FOREIGN KEY(device_id) REFERENCES devices(device_id) ON DELETE CASCADE
            );

            CREATE TABLE IF NOT EXISTS ambient_events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                duration_ms INTEGER NOT NULL,
                byte_size INTEGER NOT NULL,
                status TEXT NOT NULL CHECK(status IN ('pending', 'ignored', 'responded', 'failed')),
                transcript TEXT NOT NULL DEFAULT '',
                action TEXT NOT NULL DEFAULT '',
                response_text TEXT NOT NULL DEFAULT '',
                reason TEXT NOT NULL DEFAULT '',
                decision_engine TEXT NOT NULL DEFAULT '',
                sample_rate INTEGER NOT NULL DEFAULT 0,
                frame_duration_ms INTEGER NOT NULL DEFAULT 0,
                frame_count INTEGER NOT NULL DEFAULT 0,
                firmware_snapshot TEXT NOT NULL DEFAULT '',
                network_snapshot TEXT NOT NULL DEFAULT '',
                state_snapshot TEXT NOT NULL DEFAULT '',
                source_ip TEXT NOT NULL DEFAULT '',
                debug_recorded INTEGER NOT NULL DEFAULT 0,
                debug_raw_file TEXT NOT NULL DEFAULT '',
                debug_wav_file TEXT NOT NULL DEFAULT '',
                debug_sha256 TEXT NOT NULL DEFAULT '',
                debug_wav_bytes INTEGER NOT NULL DEFAULT 0,
                peak_dbfs REAL,
                rms_dbfs REAL,
                decode_ms INTEGER NOT NULL DEFAULT 0,
                asr_ms INTEGER NOT NULL DEFAULT 0,
                decision_ms INTEGER NOT NULL DEFAULT 0,
                tts_ms INTEGER NOT NULL DEFAULT 0,
                pipeline_ms INTEGER NOT NULL DEFAULT 0,
                debug_error TEXT NOT NULL DEFAULT '',
                created_at_ms INTEGER NOT NULL,
                processed_at_ms INTEGER,
                FOREIGN KEY(device_id) REFERENCES devices(device_id) ON DELETE CASCADE
            );
            CREATE INDEX IF NOT EXISTS idx_ambient_events_device_time
                ON ambient_events(device_id, created_at_ms DESC);

            CREATE TABLE IF NOT EXISTS speech_assets (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                ambient_event_id INTEGER NOT NULL,
                file_name TEXT NOT NULL UNIQUE,
                download_token TEXT NOT NULL UNIQUE,
                created_at_ms INTEGER NOT NULL,
                FOREIGN KEY(device_id) REFERENCES devices(device_id) ON DELETE CASCADE,
                FOREIGN KEY(ambient_event_id) REFERENCES ambient_events(id) ON DELETE CASCADE
            );
            """
        )
        command_columns = {row["name"] for row in db.execute("PRAGMA table_info(commands)")}
        if "payload_json" not in command_columns:
            db.execute("ALTER TABLE commands ADD COLUMN payload_json TEXT NOT NULL DEFAULT '{}'")
        device_columns = {row["name"] for row in db.execute("PRAGMA table_info(devices)")}
        device_migrations = {
            "network_primary": "TEXT NOT NULL DEFAULT ''",
            "network_standby": "TEXT NOT NULL DEFAULT ''",
            "automatic_failover": "INTEGER NOT NULL DEFAULT 0",
            "network_connected": "INTEGER NOT NULL DEFAULT 0",
            "ambient_enabled": "INTEGER NOT NULL DEFAULT 0",
        }
        for column, definition in device_migrations.items():
            if column not in device_columns:
                db.execute(f"ALTER TABLE devices ADD COLUMN {column} {definition}")
        ambient_setting_columns = {row["name"] for row in db.execute("PRAGMA table_info(ambient_settings)")}
        ambient_setting_migrations = {
            "debug_audio_enabled": "INTEGER NOT NULL DEFAULT 0",
            "debug_retention_days": "INTEGER NOT NULL DEFAULT 3",
            "debug_max_mb": "INTEGER NOT NULL DEFAULT 200",
        }
        for column, definition in ambient_setting_migrations.items():
            if column not in ambient_setting_columns:
                db.execute(f"ALTER TABLE ambient_settings ADD COLUMN {column} {definition}")
        ambient_event_columns = {row["name"] for row in db.execute("PRAGMA table_info(ambient_events)")}
        ambient_event_migrations = {
            "sample_rate": "INTEGER NOT NULL DEFAULT 0",
            "frame_duration_ms": "INTEGER NOT NULL DEFAULT 0",
            "frame_count": "INTEGER NOT NULL DEFAULT 0",
            "firmware_snapshot": "TEXT NOT NULL DEFAULT ''",
            "network_snapshot": "TEXT NOT NULL DEFAULT ''",
            "state_snapshot": "TEXT NOT NULL DEFAULT ''",
            "source_ip": "TEXT NOT NULL DEFAULT ''",
            "debug_recorded": "INTEGER NOT NULL DEFAULT 0",
            "debug_raw_file": "TEXT NOT NULL DEFAULT ''",
            "debug_wav_file": "TEXT NOT NULL DEFAULT ''",
            "debug_sha256": "TEXT NOT NULL DEFAULT ''",
            "debug_wav_bytes": "INTEGER NOT NULL DEFAULT 0",
            "peak_dbfs": "REAL",
            "rms_dbfs": "REAL",
            "decode_ms": "INTEGER NOT NULL DEFAULT 0",
            "asr_ms": "INTEGER NOT NULL DEFAULT 0",
            "decision_ms": "INTEGER NOT NULL DEFAULT 0",
            "tts_ms": "INTEGER NOT NULL DEFAULT 0",
            "pipeline_ms": "INTEGER NOT NULL DEFAULT 0",
            "debug_error": "TEXT NOT NULL DEFAULT ''",
        }
        for column, definition in ambient_event_migrations.items():
            if column not in ambient_event_columns:
                db.execute(f"ALTER TABLE ambient_events ADD COLUMN {column} {definition}")


@asynccontextmanager
async def lifespan(_: FastAPI):
    initialize_database()
    cleanup_ambient_debug_audio()
    yield


app = FastAPI(title="Cat Companion Cloud", version="1.3.1", lifespan=lifespan, root_path=ROOT_PATH)


def bearer_token(authorization: str | None) -> str:
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="Authorization required")
    return authorization[7:]


def verify_device(
    authorization: Annotated[str | None, Header()] = None,
    x_device_id: Annotated[str | None, Header()] = None,
) -> str:
    if not secrets.compare_digest(bearer_token(authorization), DEVICE_TOKEN):
        raise HTTPException(status_code=403, detail="Invalid device token")
    if not x_device_id or len(x_device_id) > 96:
        raise HTTPException(status_code=400, detail="Valid X-Device-Id required")
    return x_device_id


def verify_admin(x_admin_token: Annotated[str | None, Header()] = None) -> None:
    if not x_admin_token or not secrets.compare_digest(x_admin_token, ADMIN_TOKEN):
        raise HTTPException(status_code=403, detail="管理令牌无效")


def touch_device(
    device_id: str,
    *,
    name: str = "",
    firmware: str = "",
    network: str = "",
    network_primary: str = "",
    network_standby: str = "",
    automatic_failover: bool | None = None,
    network_connected: bool | None = None,
    ambient_enabled: bool | None = None,
    state: str = "",
) -> None:
    timestamp = now_ms()
    with connect_db() as db:
        existing = db.execute("SELECT * FROM devices WHERE device_id = ?", (device_id,)).fetchone()
        device_name = name.strip() or (existing["name"] if existing else device_id)
        primary = network_primary or (existing["network_primary"] if existing else "")
        standby = network_standby or (existing["network_standby"] if existing else "")
        failover = int(automatic_failover) if automatic_failover is not None else (
            existing["automatic_failover"] if existing else 0
        )
        connected = int(network_connected) if network_connected is not None else (
            existing["network_connected"] if existing else 0
        )
        ambient = int(ambient_enabled) if ambient_enabled is not None else (
            existing["ambient_enabled"] if existing else int(DEFAULT_AMBIENT_ENABLED)
        )
        db.execute(
            """
            INSERT INTO devices(
                device_id, name, firmware, network, network_primary, network_standby,
                automatic_failover, network_connected, ambient_enabled, state, last_seen_ms, created_at_ms
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(device_id) DO UPDATE SET
                name = excluded.name,
                firmware = CASE WHEN excluded.firmware = '' THEN devices.firmware ELSE excluded.firmware END,
                network = CASE WHEN excluded.network = '' THEN devices.network ELSE excluded.network END,
                network_primary = excluded.network_primary,
                network_standby = excluded.network_standby,
                automatic_failover = excluded.automatic_failover,
                network_connected = excluded.network_connected,
                ambient_enabled = excluded.ambient_enabled,
                state = CASE WHEN excluded.state = '' THEN devices.state ELSE excluded.state END,
                last_seen_ms = excluded.last_seen_ms
            """,
            (
                device_id, device_name, firmware, network, primary, standby,
                failover, connected, ambient, state, timestamp, timestamp,
            ),
        )
        db.execute(
            "INSERT OR IGNORE INTO ambient_settings(device_id, enabled, debug_audio_enabled, "
            "debug_retention_days, debug_max_mb, updated_at_ms) VALUES (?, ?, ?, ?, ?, ?)",
            (
                device_id, ambient, int(DEFAULT_AMBIENT_DEBUG_AUDIO),
                DEFAULT_AMBIENT_DEBUG_RETENTION_DAYS, DEFAULT_AMBIENT_DEBUG_MAX_MB, timestamp,
            ),
        )


def ambient_settings(device_id: str, db: sqlite3.Connection | None = None) -> dict:
    owns_connection = db is None
    connection = db or connect_db()
    try:
        row = connection.execute(
            "SELECT enabled, cooldown_seconds, max_per_hour, quiet_start, quiet_end, "
            "debug_audio_enabled, debug_retention_days, debug_max_mb, updated_at_ms "
            "FROM ambient_settings WHERE device_id = ?",
            (device_id,),
        ).fetchone()
        if row is None:
            timestamp = now_ms()
            connection.execute(
                "INSERT INTO ambient_settings(device_id, enabled, debug_audio_enabled, debug_retention_days, "
                "debug_max_mb, updated_at_ms) VALUES (?, ?, ?, ?, ?, ?)",
                (
                    device_id, int(DEFAULT_AMBIENT_ENABLED), int(DEFAULT_AMBIENT_DEBUG_AUDIO),
                    DEFAULT_AMBIENT_DEBUG_RETENTION_DAYS, DEFAULT_AMBIENT_DEBUG_MAX_MB, timestamp,
                ),
            )
            row = connection.execute(
                "SELECT enabled, cooldown_seconds, max_per_hour, quiet_start, quiet_end, "
                "debug_audio_enabled, debug_retention_days, debug_max_mb, updated_at_ms "
                "FROM ambient_settings WHERE device_id = ?",
                (device_id,),
            ).fetchone()
        result = dict(row)
        result["enabled"] = bool(result["enabled"])
        result["debug_audio_enabled"] = bool(result["debug_audio_enabled"])
        return result
    finally:
        if owns_connection:
            connection.commit()
            connection.close()


def public_origin(request: Request) -> str:
    origin = str(request.base_url).rstrip("/")
    if ROOT_PATH and not origin.endswith(ROOT_PATH):
        origin += ROOT_PATH
    return origin


def clean_transcript(text: str) -> str:
    return re.sub(r"<\|.*?\|>", "", text).strip()


def is_quiet_time(start: str, end: str) -> bool:
    current = datetime.now(LOCAL_TIMEZONE).strftime("%H:%M")
    if start == end:
        return False
    if start < end:
        return start <= current < end
    return current >= start or current < end


class Heartbeat(BaseModel):
    name: str = Field(default="", max_length=80)
    firmware: str = Field(default="", max_length=64)
    network: str = Field(default="", max_length=24)
    network_primary: str = Field(default="", max_length=24)
    network_standby: str = Field(default="", max_length=24)
    automatic_failover: bool | None = None
    network_connected: bool | None = None
    ambient_enabled: bool | None = None
    state: str = Field(default="", max_length=32)


class ChatEvent(BaseModel):
    session_id: str = Field(default="", max_length=96)
    role: Literal["user", "assistant"]
    content: str = Field(min_length=1, max_length=4000)
    occurred_at_ms: int = 0


class CommandCompletion(BaseModel):
    status: Literal["completed", "failed"]
    photo_id: int | None = None
    error: str = Field(default="", max_length=500)


class GatewayRequest(BaseModel):
    action: str = Field(min_length=1, max_length=64, pattern=r"^[A-Za-z0-9_.-]+$")
    request: str = Field(default="", max_length=1200)
    parameters: str = Field(default="{}", max_length=1600)


class OtaQueueRequest(BaseModel):
    release_id: int = Field(gt=0)


class AmbientSettingsUpdate(BaseModel):
    enabled: bool
    cooldown_seconds: int = Field(default=90, ge=30, le=3600)
    max_per_hour: int = Field(default=3, ge=1, le=20)
    quiet_start: str = Field(default="23:00", pattern=r"^(?:[01]\d|2[0-3]):[0-5]\d$")
    quiet_end: str = Field(default="07:00", pattern=r"^(?:[01]\d|2[0-3]):[0-5]\d$")
    debug_audio_enabled: bool = False
    debug_retention_days: int = Field(default=3, ge=1, le=30)
    debug_max_mb: int = Field(default=200, ge=20, le=2048)


@app.get("/health")
def health() -> dict:
    return {
        "ok": True,
        "service": "cat-companion-cloud",
        "version": app.version,
        "speech": speech_engine.status(),
        "decision": decision_engine.status(),
    }


@app.post("/api/v1/device/heartbeat")
def device_heartbeat(payload: Heartbeat, device_id: str = Depends(verify_device)) -> dict:
    touch_device(
        device_id,
        name=payload.name,
        firmware=payload.firmware,
        network=payload.network,
        network_primary=payload.network_primary,
        network_standby=payload.network_standby,
        automatic_failover=payload.automatic_failover,
        network_connected=payload.network_connected,
        ambient_enabled=payload.ambient_enabled,
        state=payload.state,
    )
    settings = ambient_settings(device_id)
    speech = speech_engine.status()
    return {
        "ok": True,
        "server_time_ms": now_ms(),
        "ambient": {
            "supported": True,
            "enabled": settings["enabled"],
            "speech_ready": speech["ready"],
            "cooldown_seconds": settings["cooldown_seconds"],
            "max_per_hour": settings["max_per_hour"],
        },
    }


@app.post("/api/v1/device/chats", status_code=201)
def create_chat(payload: ChatEvent, device_id: str = Depends(verify_device)) -> dict:
    touch_device(device_id)
    created_at = now_ms()
    occurred_at = normalize_device_timestamp(payload.occurred_at_ms, created_at)
    with connect_db() as db:
        cursor = db.execute(
            """
            INSERT INTO chats(device_id, session_id, role, content, occurred_at_ms, created_at_ms)
            VALUES (?, ?, ?, ?, ?, ?)
            """,
            (device_id, payload.session_id, payload.role, payload.content, occurred_at, created_at),
        )
    return {"ok": True, "id": cursor.lastrowid}


def cleanup_speech_assets() -> None:
    cutoff = now_ms() - SPEECH_ASSET_TTL_MS
    with connect_db() as db:
        rows = db.execute(
            "SELECT id, file_name FROM speech_assets WHERE created_at_ms < ?", (cutoff,)
        ).fetchall()
        for row in rows:
            (SPEECH_DIR / row["file_name"]).unlink(missing_ok=True)
            db.execute("DELETE FROM speech_assets WHERE id = ?", (row["id"],))


def ambient_debug_path(file_name: str) -> Path | None:
    if not file_name:
        return None
    path = (AMBIENT_DEBUG_DIR / file_name).resolve()
    return path if path.parent == AMBIENT_DEBUG_DIR.resolve() else None


def ambient_debug_file_size(file_name: str) -> int:
    path = ambient_debug_path(file_name)
    return path.stat().st_size if path is not None and path.is_file() else 0


def delete_ambient_debug_files(row: sqlite3.Row | dict) -> None:
    for column in ("debug_raw_file", "debug_wav_file"):
        path = ambient_debug_path(row[column])
        if path is not None:
            path.unlink(missing_ok=True)


def cleanup_ambient_debug_audio(device_id: str = "") -> None:
    where = "WHERE (e.debug_raw_file != '' OR e.debug_wav_file != '')"
    if device_id:
        where += " AND e.device_id = ?"
    params: tuple = (device_id,) if device_id else ()
    with connect_db() as db:
        rows = db.execute(
            "SELECT e.id, e.device_id, e.created_at_ms, e.debug_raw_file, e.debug_wav_file, "
            "COALESCE(s.debug_retention_days, ?) AS retention_days, "
            "COALESCE(s.debug_max_mb, ?) AS max_mb FROM ambient_events e "
            "LEFT JOIN ambient_settings s ON s.device_id = e.device_id "
            f"{where} ORDER BY e.device_id, e.created_at_ms DESC",
            (DEFAULT_AMBIENT_DEBUG_RETENTION_DAYS, DEFAULT_AMBIENT_DEBUG_MAX_MB, *params),
        ).fetchall()
        grouped: dict[str, list[sqlite3.Row]] = {}
        for row in rows:
            grouped.setdefault(row["device_id"], []).append(row)

        current_time = now_ms()
        for device_rows in grouped.values():
            retained_bytes = 0
            for row in device_rows:
                size = ambient_debug_file_size(row["debug_raw_file"]) + ambient_debug_file_size(row["debug_wav_file"])
                expired = current_time - row["created_at_ms"] > row["retention_days"] * 24 * 60 * 60 * 1000
                over_quota = retained_bytes + size > row["max_mb"] * 1024 * 1024
                if size and (expired or over_quota):
                    delete_ambient_debug_files(row)
                    db.execute(
                        "UPDATE ambient_events SET debug_raw_file = '', debug_wav_file = '', "
                        "debug_wav_bytes = 0 WHERE id = ?",
                        (row["id"],),
                    )
                else:
                    retained_bytes += size


def ambient_debug_storage(device_id: str) -> dict:
    with connect_db() as db:
        rows = db.execute(
            "SELECT debug_raw_file, debug_wav_file FROM ambient_events WHERE device_id = ? "
            "AND (debug_raw_file != '' OR debug_wav_file != '')",
            (device_id,),
        ).fetchall()
    byte_size = sum(
        ambient_debug_file_size(row["debug_raw_file"]) + ambient_debug_file_size(row["debug_wav_file"])
        for row in rows
    )
    return {"bytes": byte_size, "megabytes": round(byte_size / (1024 * 1024), 2)}


def update_ambient_diagnostics(event_id: int, **values: object) -> None:
    allowed = {
        "debug_raw_file", "debug_wav_file", "debug_sha256", "debug_wav_bytes", "peak_dbfs", "rms_dbfs",
        "decode_ms", "asr_ms", "decision_ms", "tts_ms", "pipeline_ms", "debug_error",
    }
    updates = [(key, value) for key, value in values.items() if key in allowed]
    if not updates:
        return
    assignments = ", ".join(f"{key} = ?" for key, _ in updates)
    with connect_db() as db:
        db.execute(
            f"UPDATE ambient_events SET {assignments} WHERE id = ?",
            (*[value for _, value in updates], event_id),
        )


def save_ambient_debug_raw(event_id: int, content: bytes) -> tuple[str, str]:
    file_name = f"ambient-{event_id}-{uuid.uuid4().hex}.cap1"
    final_path = AMBIENT_DEBUG_DIR / file_name
    temporary_path = AMBIENT_DEBUG_DIR / f".{file_name}.part"
    try:
        temporary_path.write_bytes(content)
        temporary_path.replace(final_path)
    except Exception:
        temporary_path.unlink(missing_ok=True)
        raise
    return file_name, hashlib.sha256(content).hexdigest()


def source_ip(request: Request) -> str:
    forwarded = request.headers.get("x-forwarded-for", "").split(",", 1)[0].strip()
    client = request.client.host if request.client is not None else ""
    return (forwarded or client)[:64]


def serialize_ambient_event(row: sqlite3.Row) -> dict:
    event = dict(row)
    raw_file = event.pop("debug_raw_file", "")
    wav_file = event.pop("debug_wav_file", "")
    event["debug_recorded"] = bool(event["debug_recorded"])
    event["has_debug_raw"] = ambient_debug_file_size(raw_file) > 0
    event["has_debug_wav"] = ambient_debug_file_size(wav_file) > 0
    event["debug_storage_bytes"] = ambient_debug_file_size(raw_file) + ambient_debug_file_size(wav_file)
    return event


def finish_ambient_event(event_id: int, *, status: str, transcript: str = "", action: str = "",
                         response_text: str = "", reason: str = "", engine: str = "",
                         pipeline_ms: int = 0) -> None:
    with connect_db() as db:
        db.execute(
            "UPDATE ambient_events SET status = ?, transcript = ?, action = ?, response_text = ?, "
            "reason = ?, decision_engine = ?, pipeline_ms = ?, processed_at_ms = ? WHERE id = ?",
            (status, transcript[:2000], action[:32], response_text[:500], reason[:500],
             engine[:64], pipeline_ms, now_ms(), event_id),
        )


def process_ambient_event(event_id: int, device_id: str, content: bytes, origin: str) -> None:
    transcript = ""
    pipeline_started = time.perf_counter()

    def elapsed_ms(started: float) -> int:
        return max(0, round((time.perf_counter() - started) * 1000))

    with AMBIENT_PROCESS_LOCK:
        try:
            decode_started = time.perf_counter()
            pcm, sample_rate, _ = decode_cat_opus(content)
            diagnostics = pcm_diagnostics(pcm)
            update_ambient_diagnostics(
                event_id,
                peak_dbfs=diagnostics["peak_dbfs"],
                rms_dbfs=diagnostics["rms_dbfs"],
                decode_ms=elapsed_ms(decode_started),
            )

            with connect_db() as db:
                event_row = db.execute(
                    "SELECT debug_recorded FROM ambient_events WHERE id = ?", (event_id,)
                ).fetchone()
            if event_row is not None and event_row["debug_recorded"]:
                try:
                    wav_name = f"ambient-{event_id}-{uuid.uuid4().hex}.wav"
                    final_wav = AMBIENT_DEBUG_DIR / wav_name
                    temporary_wav = AMBIENT_DEBUG_DIR / f".{wav_name}.part.wav"
                    write_pcm_wav(temporary_wav, pcm, sample_rate)
                    temporary_wav.replace(final_wav)
                    update_ambient_diagnostics(
                        event_id, debug_wav_file=wav_name, debug_wav_bytes=final_wav.stat().st_size
                    )
                except Exception as exc:
                    if "temporary_wav" in locals():
                        temporary_wav.unlink(missing_ok=True)
                    update_ambient_diagnostics(event_id, debug_error=f"WAV 保存失败：{exc}"[:500])

            asr_started = time.perf_counter()
            transcript = clean_transcript(speech_engine.transcribe(pcm, sample_rate))
            update_ambient_diagnostics(event_id, asr_ms=elapsed_ms(asr_started))
            if len(re.sub(r"\s+", "", transcript)) < 4:
                finish_ambient_event(
                    event_id, status="ignored", transcript=transcript, action="ignore",
                    reason="未识别到有效语句", engine="asr-filter",
                    pipeline_ms=elapsed_ms(pipeline_started),
                )
                return

            with connect_db() as db:
                settings = ambient_settings(device_id, db)
                active_command = db.execute(
                    "SELECT id FROM commands WHERE device_id = ? AND command = 'proactive_speak' "
                    "AND status IN ('queued', 'running') LIMIT 1",
                    (device_id,),
                ).fetchone()
                last_response = db.execute(
                    "SELECT processed_at_ms FROM ambient_events WHERE device_id = ? AND status = 'responded' "
                    "AND id != ? ORDER BY processed_at_ms DESC LIMIT 1",
                    (device_id, event_id),
                ).fetchone()
                hourly_count = db.execute(
                    "SELECT COUNT(*) AS count FROM ambient_events WHERE device_id = ? "
                    "AND status = 'responded' AND processed_at_ms >= ?",
                    (device_id, now_ms() - 60 * 60 * 1000),
                ).fetchone()["count"]
                recent = [dict(row) for row in db.execute(
                    "SELECT role, content FROM chats WHERE device_id = ? "
                    "ORDER BY occurred_at_ms DESC LIMIT 6",
                    (device_id,),
                ).fetchall()]

            blocked_reason = ""
            if not settings["enabled"]:
                blocked_reason = "主动聆听已关闭"
            elif is_quiet_time(settings["quiet_start"], settings["quiet_end"]):
                blocked_reason = "当前处于安静时段"
            elif active_command is not None:
                blocked_reason = "已有主动播报等待设备执行"
            elif last_response is not None and now_ms() - last_response["processed_at_ms"] < settings["cooldown_seconds"] * 1000:
                blocked_reason = "仍在主动互动冷却时间内"
            elif hourly_count >= settings["max_per_hour"]:
                blocked_reason = "已达到每小时主动互动上限"
            if blocked_reason:
                finish_ambient_event(
                    event_id, status="ignored", transcript=transcript, action="ignore",
                    reason=blocked_reason, engine="rate-limit",
                    pipeline_ms=elapsed_ms(pipeline_started),
                )
                return

            decision_started = time.perf_counter()
            decision = decision_engine.decide(transcript, list(reversed(recent)))
            update_ambient_diagnostics(event_id, decision_ms=elapsed_ms(decision_started))
            if decision.action == "ignore":
                finish_ambient_event(
                    event_id, status="ignored", transcript=transcript, action=decision.action,
                    reason=decision.reason, engine=decision.engine,
                    pipeline_ms=elapsed_ms(pipeline_started),
                )
                return

            file_name = f"speech-{event_id}-{uuid.uuid4().hex}.wav"
            final_path = SPEECH_DIR / file_name
            temporary_path = SPEECH_DIR / f".{file_name}.part.wav"
            tts_started = time.perf_counter()
            speech_engine.synthesize(decision.response, temporary_path)
            temporary_path.replace(final_path)
            update_ambient_diagnostics(event_id, tts_ms=elapsed_ms(tts_started))
            token = secrets.token_urlsafe(32)
            try:
                with connect_db() as db:
                    cursor = db.execute(
                        "INSERT INTO speech_assets(device_id, ambient_event_id, file_name, download_token, created_at_ms) "
                        "VALUES (?, ?, ?, ?, ?)",
                        (device_id, event_id, file_name, token, now_ms()),
                    )
                    audio_url = f"{origin}/api/v1/device/speech/{cursor.lastrowid}?token={token}"
                    payload = json.dumps(
                        {"text": decision.response, "audio_url": audio_url, "event_id": event_id},
                        ensure_ascii=False, separators=(",", ":"),
                    )
                    db.execute(
                        "INSERT INTO commands(device_id, command, payload_json, status, requested_at_ms) "
                        "VALUES (?, 'proactive_speak', ?, 'queued', ?)",
                        (device_id, payload, now_ms()),
                    )
            except Exception:
                final_path.unlink(missing_ok=True)
                raise
            finish_ambient_event(
                event_id, status="responded", transcript=transcript, action=decision.action,
                response_text=decision.response, reason=decision.reason, engine=decision.engine,
                pipeline_ms=elapsed_ms(pipeline_started),
            )
            cleanup_speech_assets()
        except Exception as exc:
            finish_ambient_event(
                event_id, status="failed", transcript=transcript, action="error",
                reason=str(exc), engine="pipeline", pipeline_ms=elapsed_ms(pipeline_started),
            )
        finally:
            cleanup_ambient_debug_audio(device_id)


@app.post("/api/v1/device/ambient-audio", status_code=202)
async def create_ambient_audio(
    request: Request,
    background_tasks: BackgroundTasks,
    device_id: str = Depends(verify_device),
) -> dict:
    content_type = request.headers.get("content-type", "").split(";", 1)[0].strip().lower()
    if content_type != "application/x-cat-opus":
        raise HTTPException(status_code=415, detail="application/x-cat-opus required")
    content = await request.body()
    if not content or len(content) > MAX_AMBIENT_AUDIO_BYTES:
        raise HTTPException(status_code=413, detail="Ambient audio is empty or too large")
    try:
        sample_rate, frame_duration_ms, packets = inspect_cat_opus(content)
    except AmbientAudioError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    duration_ms = frame_duration_ms * len(packets)
    if duration_ms < MIN_AMBIENT_DURATION_MS or duration_ms > MAX_AMBIENT_DURATION_MS:
        raise HTTPException(status_code=422, detail="Ambient speech duration is outside the allowed range")
    if not speech_engine.status()["ready"]:
        raise HTTPException(status_code=503, detail="Speech engine is not ready")

    touch_device(device_id)
    with connect_db() as db:
        settings = ambient_settings(device_id, db)
        if not settings["enabled"]:
            raise HTTPException(status_code=409, detail="Ambient listening is disabled")
        pending = db.execute(
            "SELECT COUNT(*) AS count FROM ambient_events WHERE device_id = ? AND status = 'pending'",
            (device_id,),
        ).fetchone()["count"]
        if pending >= 2:
            raise HTTPException(status_code=429, detail="Ambient processing queue is full")
        device = db.execute(
            "SELECT firmware, network, state FROM devices WHERE device_id = ?", (device_id,)
        ).fetchone()
        cursor = db.execute(
            "INSERT INTO ambient_events(device_id, duration_ms, byte_size, status, sample_rate, "
            "frame_duration_ms, frame_count, firmware_snapshot, network_snapshot, state_snapshot, "
            "source_ip, debug_recorded, debug_sha256, created_at_ms) "
            "VALUES (?, ?, ?, 'pending', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (
                device_id, duration_ms, len(content), sample_rate, frame_duration_ms, len(packets),
                device["firmware"] if device else "", device["network"] if device else "",
                device["state"] if device else "", source_ip(request), int(settings["debug_audio_enabled"]),
                hashlib.sha256(content).hexdigest(), now_ms(),
            ),
        )
        event_id = cursor.lastrowid
    if settings["debug_audio_enabled"]:
        try:
            raw_name, digest = save_ambient_debug_raw(event_id, content)
            update_ambient_diagnostics(event_id, debug_raw_file=raw_name, debug_sha256=digest)
        except Exception as exc:
            update_ambient_diagnostics(event_id, debug_error=f"原始音频保存失败：{exc}"[:500])
    cleanup_ambient_debug_audio(device_id)
    background_tasks.add_task(process_ambient_event, event_id, device_id, content, public_origin(request))
    return {
        "ok": True,
        "event_id": event_id,
        "duration_ms": duration_ms,
        "debug_recorded": settings["debug_audio_enabled"],
    }


@app.get("/api/v1/device/speech/{asset_id}")
def download_speech(asset_id: int, token: str = Query(min_length=24, max_length=128)) -> FileResponse:
    with connect_db() as db:
        row = db.execute(
            "SELECT file_name, download_token, created_at_ms FROM speech_assets WHERE id = ?", (asset_id,)
        ).fetchone()
    if row is None or now_ms() - row["created_at_ms"] > SPEECH_ASSET_TTL_MS:
        raise HTTPException(status_code=404, detail="Speech audio not found")
    if not secrets.compare_digest(token, row["download_token"]):
        raise HTTPException(status_code=404, detail="Speech audio not found")
    path = SPEECH_DIR / row["file_name"]
    if not path.is_file():
        raise HTTPException(status_code=404, detail="Speech audio file not found")
    return FileResponse(path, media_type="audio/wav", filename="companion-speech.wav")


@app.post("/api/v1/device/gateway")
def private_gateway(payload: GatewayRequest, device_id: str = Depends(verify_device)) -> dict:
    touch_device(device_id)
    action = payload.action
    if action == "help":
        return {
            "ok": True,
            "actions": [
                "help", "status", "recent_chats", "recent_photos", "latest_photo",
                "ambient_status", "recent_ambient", "ota_status",
            ],
        }

    with connect_db() as db:
        if action == "status":
            device = db.execute(
                "SELECT name, firmware, network, network_primary, network_standby, "
                "automatic_failover, network_connected, state, last_seen_ms "
                "FROM devices WHERE device_id = ?",
                (device_id,),
            ).fetchone()
            counts = db.execute(
                "SELECT (SELECT COUNT(*) FROM chats WHERE device_id = ?) AS chats, "
                "(SELECT COUNT(*) FROM photos WHERE device_id = ?) AS photos, "
                "(SELECT COUNT(*) FROM ambient_events WHERE device_id = ?) AS ambient_events",
                (device_id, device_id, device_id),
            ).fetchone()
            return {"ok": True, "device": dict(device), "records": dict(counts)}
        if action == "recent_chats":
            rows = db.execute(
                "SELECT role, content, occurred_at_ms FROM chats WHERE device_id = ? "
                "ORDER BY occurred_at_ms DESC LIMIT 5",
                (device_id,),
            ).fetchall()
            return {"ok": True, "chats": [dict(row) for row in rows]}
        if action in {"recent_photos", "latest_photo"}:
            limit = 1 if action == "latest_photo" else 5
            rows = db.execute(
                "SELECT id, source, prompt, analysis, width, height, captured_at_ms "
                "FROM photos WHERE device_id = ? ORDER BY captured_at_ms DESC LIMIT ?",
                (device_id, limit),
            ).fetchall()
            return {"ok": True, "photos": [dict(row) for row in rows]}
        if action == "ota_status":
            row = db.execute(
                "SELECT id, status, requested_at_ms, started_at_ms, completed_at_ms, error "
                "FROM commands WHERE device_id = ? AND command = 'firmware_update' "
                "ORDER BY requested_at_ms DESC LIMIT 1",
                (device_id,),
            ).fetchone()
            return {"ok": True, "ota": dict(row) if row else None}
        if action == "ambient_status":
            return {
                "ok": True,
                "settings": ambient_settings(device_id, db),
                "speech": speech_engine.status(),
                "decision": decision_engine.status(),
            }
        if action == "recent_ambient":
            rows = db.execute(
                "SELECT transcript, status, action, response_text, reason, created_at_ms "
                "FROM ambient_events WHERE device_id = ? ORDER BY created_at_ms DESC LIMIT 5",
                (device_id,),
            ).fetchall()
            return {"ok": True, "events": [dict(row) for row in rows]}

    raise HTTPException(status_code=400, detail="Unsupported gateway action")


@app.get("/api/v1/device/commands/next")
async def next_command(
    wait_seconds: int = Query(default=20, ge=0, le=25),
    device_id: str = Depends(verify_device),
) -> dict:
    deadline = time.monotonic() + wait_seconds
    while True:
        touch_device(device_id)
        with connect_db() as db:
            # A response can be lost after the database claim reaches a 4G
            # device. Reissue the same command after a bounded lease instead
            # of leaving it permanently stuck in the running state.
            timestamp = now_ms()
            db.execute(
                """
                UPDATE commands
                SET status = 'queued', started_at_ms = NULL,
                    error = 'delivery lease expired; command requeued'
                WHERE device_id = ? AND status = 'running'
                  AND started_at_ms IS NOT NULL AND started_at_ms <= ?
                """,
                (device_id, timestamp - COMMAND_LEASE_MS),
            )
            row = db.execute(
                """
                SELECT id, command, payload_json, requested_at_ms
                FROM commands
                WHERE device_id = ? AND status = 'queued'
                ORDER BY requested_at_ms ASC
                LIMIT 1
                """,
                (device_id,),
            ).fetchone()
            if row is not None:
                updated = db.execute(
                    "UPDATE commands SET status = 'running', started_at_ms = ? WHERE id = ? AND status = 'queued'",
                    (timestamp, row["id"]),
                )
                if updated.rowcount == 1:
                    command = dict(row)
                    try:
                        command["payload"] = json.loads(command.pop("payload_json") or "{}")
                    except json.JSONDecodeError:
                        command["payload"] = {}
                    return {"ok": True, "command": command}
        if time.monotonic() >= deadline:
            return {"ok": True, "command": None}
        await asyncio.sleep(0.5)


@app.post("/api/v1/device/commands/{command_id}/complete")
def complete_command(
    command_id: int,
    payload: CommandCompletion,
    device_id: str = Depends(verify_device),
) -> dict:
    with connect_db() as db:
        updated = db.execute(
            """
            UPDATE commands
            SET status = ?, completed_at_ms = ?, photo_id = ?, error = ?
            WHERE id = ? AND device_id = ? AND status IN ('queued', 'running')
            """,
            (payload.status, now_ms(), payload.photo_id, payload.error, command_id, device_id),
        )
    if updated.rowcount != 1:
        raise HTTPException(status_code=404, detail="Active command not found")
    return {"ok": True}


@app.post("/api/v1/device/photos", status_code=201)
async def create_photo(
    image: Annotated[UploadFile, File()],
    source: Annotated[Literal["voice", "admin"], Form()],
    prompt: Annotated[str, Form()] = "",
    analysis: Annotated[str, Form()] = "",
    width: Annotated[int, Form()] = 0,
    height: Annotated[int, Form()] = 0,
    captured_at_ms: Annotated[int, Form()] = 0,
    command_id: Annotated[int | None, Form()] = None,
    device_id: str = Depends(verify_device),
) -> dict:
    if image.content_type not in {"image/jpeg", "image/png"}:
        raise HTTPException(status_code=415, detail="JPEG or PNG required")
    content = await image.read(MAX_PHOTO_BYTES + 1)
    if not content or len(content) > MAX_PHOTO_BYTES:
        raise HTTPException(status_code=413, detail="Photo size must be between 1 byte and 2 MB")
    if source == "admin" and command_id is None:
        raise HTTPException(status_code=400, detail="Admin photo requires command_id")
    if source == "admin":
        with connect_db() as db:
            command = db.execute(
                """
                SELECT id FROM commands
                WHERE id = ? AND device_id = ? AND status IN ('queued', 'running')
                """,
                (command_id, device_id),
            ).fetchone()
        if command is None:
            raise HTTPException(status_code=409, detail="Active capture command required")

    touch_device(device_id)
    suffix = ".jpg" if image.content_type == "image/jpeg" else ".png"
    file_name = f"{safe_file_stem(device_id)}-{uuid.uuid4().hex}{suffix}"
    final_path = PHOTO_DIR / file_name
    temporary_path = PHOTO_DIR / f".{file_name}.part"
    temporary_path.write_bytes(content)
    temporary_path.replace(final_path)

    created_at = now_ms()
    captured_at = normalize_device_timestamp(captured_at_ms, created_at)
    try:
        with connect_db() as db:
            cursor = db.execute(
                """
                INSERT INTO photos(
                    device_id, source, file_name, content_type, byte_size, width, height,
                    prompt, analysis, command_id, captured_at_ms, created_at_ms
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    device_id, source, file_name, image.content_type, len(content), width, height,
                    prompt[:1000], analysis[:4000], command_id, captured_at, created_at,
                ),
            )
            photo_id = cursor.lastrowid
            if command_id is not None:
                db.execute(
                    """
                    UPDATE commands
                    SET status = 'completed', completed_at_ms = ?, photo_id = ?, error = ''
                    WHERE id = ? AND device_id = ? AND status IN ('queued', 'running')
                    """,
                    (now_ms(), photo_id, command_id, device_id),
                )
    except Exception:
        final_path.unlink(missing_ok=True)
        raise
    return {"ok": True, "photo_id": photo_id}


@app.get("/api/v1/admin/devices", dependencies=[Depends(verify_admin)])
def list_devices() -> dict:
    current = now_ms()
    with connect_db() as db:
        rows = db.execute(
            """
            SELECT d.*,
                   (SELECT COUNT(*) FROM chats c WHERE c.device_id = d.device_id) AS chat_count,
                   (SELECT COUNT(*) FROM photos p WHERE p.device_id = d.device_id) AS photo_count,
                   (SELECT COUNT(*) FROM ambient_events e WHERE e.device_id = d.device_id) AS ambient_event_count,
                   COALESCE((SELECT enabled FROM ambient_settings s WHERE s.device_id = d.device_id), 0)
                       AS ambient_server_enabled
            FROM devices d ORDER BY d.last_seen_ms DESC
            """
        ).fetchall()
    devices = []
    for row in rows:
        item = dict(row)
        item["online"] = current - item["last_seen_ms"] <= ONLINE_WINDOW_SECONDS * 1000
        devices.append(item)
    return {"ok": True, "devices": devices, "online_window_seconds": ONLINE_WINDOW_SECONDS}


@app.post("/api/v1/admin/devices/{device_id}/capture", status_code=202, dependencies=[Depends(verify_admin)])
def request_capture(device_id: str) -> dict:
    with connect_db() as db:
        device = db.execute("SELECT last_seen_ms FROM devices WHERE device_id = ?", (device_id,)).fetchone()
        if device is None:
            raise HTTPException(status_code=404, detail="Device not found")
        if now_ms() - device["last_seen_ms"] > ONLINE_WINDOW_SECONDS * 1000:
            raise HTTPException(status_code=409, detail="Device is offline")
        active = db.execute(
            "SELECT id FROM commands WHERE device_id = ? AND command = 'capture' AND status IN ('queued', 'running')",
            (device_id,),
        ).fetchone()
        if active is not None:
            return {"ok": True, "command_id": active["id"], "reused": True}
        cursor = db.execute(
            "INSERT INTO commands(device_id, command, status, requested_at_ms) VALUES (?, 'capture', 'queued', ?)",
            (device_id, now_ms()),
        )
    return {"ok": True, "command_id": cursor.lastrowid, "reused": False}


@app.post("/api/v1/admin/devices/{device_id}/greet", status_code=202, dependencies=[Depends(verify_admin)])
def request_greeting(device_id: str) -> dict:
    greeting = "你在干嘛呀？"
    with connect_db() as db:
        device = db.execute("SELECT last_seen_ms FROM devices WHERE device_id = ?", (device_id,)).fetchone()
        if device is None:
            raise HTTPException(status_code=404, detail="Device not found")
        if now_ms() - device["last_seen_ms"] > ONLINE_WINDOW_SECONDS * 1000:
            raise HTTPException(status_code=409, detail="Device is offline")
        active = db.execute(
            "SELECT id FROM commands WHERE device_id = ? AND command = 'greet' AND status IN ('queued', 'running')",
            (device_id,),
        ).fetchone()
        if active is not None:
            return {"ok": True, "command_id": active["id"], "reused": True, "message": greeting}
        cursor = db.execute(
            "INSERT INTO commands(device_id, command, payload_json, status, requested_at_ms) "
            "VALUES (?, 'greet', ?, 'queued', ?)",
            (device_id, json.dumps({"message": greeting}, ensure_ascii=False), now_ms()),
        )
    return {"ok": True, "command_id": cursor.lastrowid, "reused": False, "message": greeting}


@app.post("/api/v1/admin/devices/{device_id}/ambient", dependencies=[Depends(verify_admin)])
def update_ambient_settings(device_id: str, payload: AmbientSettingsUpdate) -> dict:
    timestamp = now_ms()
    command_payload = json.dumps(
        {"enabled": payload.enabled}, ensure_ascii=False, separators=(",", ":")
    )
    with connect_db() as db:
        device = db.execute("SELECT device_id FROM devices WHERE device_id = ?", (device_id,)).fetchone()
        if device is None:
            raise HTTPException(status_code=404, detail="Device not found")
        db.execute(
            "INSERT INTO ambient_settings(device_id, enabled, cooldown_seconds, max_per_hour, "
            "quiet_start, quiet_end, debug_audio_enabled, debug_retention_days, debug_max_mb, updated_at_ms) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(device_id) DO UPDATE SET enabled = excluded.enabled, "
            "cooldown_seconds = excluded.cooldown_seconds, max_per_hour = excluded.max_per_hour, "
            "quiet_start = excluded.quiet_start, quiet_end = excluded.quiet_end, "
            "debug_audio_enabled = excluded.debug_audio_enabled, "
            "debug_retention_days = excluded.debug_retention_days, debug_max_mb = excluded.debug_max_mb, "
            "updated_at_ms = excluded.updated_at_ms",
            (
                device_id, int(payload.enabled), payload.cooldown_seconds, payload.max_per_hour,
                payload.quiet_start, payload.quiet_end, int(payload.debug_audio_enabled),
                payload.debug_retention_days, payload.debug_max_mb, timestamp,
            ),
        )
        db.execute("UPDATE devices SET ambient_enabled = ? WHERE device_id = ?", (int(payload.enabled), device_id))
        queued = db.execute(
            "SELECT id FROM commands WHERE device_id = ? AND command = 'ambient_config' "
            "AND status = 'queued' ORDER BY requested_at_ms DESC LIMIT 1",
            (device_id,),
        ).fetchone()
        if queued is not None:
            db.execute(
                "UPDATE commands SET payload_json = ?, requested_at_ms = ?, error = '' WHERE id = ?",
                (command_payload, timestamp, queued["id"]),
            )
            command_id = queued["id"]
            reused = True
        else:
            cursor = db.execute(
                "INSERT INTO commands(device_id, command, payload_json, status, requested_at_ms) "
                "VALUES (?, 'ambient_config', ?, 'queued', ?)",
                (device_id, command_payload, timestamp),
            )
            command_id = cursor.lastrowid
            reused = False
    cleanup_ambient_debug_audio(device_id)
    return {
        "ok": True,
        "settings": payload.model_dump(),
        "debug_storage": ambient_debug_storage(device_id),
        "command_id": command_id,
        "reused": reused,
    }


@app.get("/api/v1/admin/devices/{device_id}/ambient", dependencies=[Depends(verify_admin)])
def get_ambient_settings(device_id: str) -> dict:
    with connect_db() as db:
        device = db.execute("SELECT device_id FROM devices WHERE device_id = ?", (device_id,)).fetchone()
        if device is None:
            raise HTTPException(status_code=404, detail="Device not found")
        settings = ambient_settings(device_id, db)
    return {
        "ok": True,
        "settings": settings,
        "debug_storage": ambient_debug_storage(device_id),
        "speech": speech_engine.status(),
        "decision": decision_engine.status(),
    }


@app.get("/api/v1/admin/ambient-events", dependencies=[Depends(verify_admin)])
def list_ambient_events(device_id: str = "", limit: int = Query(default=100, ge=1, le=500)) -> dict:
    where = "WHERE device_id = ?" if device_id else ""
    params: tuple = (device_id, limit) if device_id else (limit,)
    with connect_db() as db:
        rows = db.execute(
            f"SELECT * FROM ambient_events {where} ORDER BY created_at_ms DESC LIMIT ?", params
        ).fetchall()
    return {
        "ok": True,
        "events": [serialize_ambient_event(row) for row in rows],
        "speech": speech_engine.status(),
        "decision": decision_engine.status(),
    }


@app.get("/api/v1/admin/ambient-events/{event_id}/audio", dependencies=[Depends(verify_admin)])
def download_ambient_debug_audio(
    event_id: int,
    kind: Literal["wav", "raw"] = Query(default="wav"),
) -> FileResponse:
    column = "debug_wav_file" if kind == "wav" else "debug_raw_file"
    with connect_db() as db:
        row = db.execute(f"SELECT {column} AS file_name FROM ambient_events WHERE id = ?", (event_id,)).fetchone()
    if row is None:
        raise HTTPException(status_code=404, detail="主动互动记录不存在")
    path = ambient_debug_path(row["file_name"])
    if path is None or not path.is_file():
        raise HTTPException(status_code=404, detail="调试音频不存在或已按保留策略清理")
    suffix = "wav" if kind == "wav" else "cap1"
    media_type = "audio/wav" if kind == "wav" else "application/octet-stream"
    return FileResponse(
        path,
        media_type=media_type,
        filename=f"ambient-{event_id}.{suffix}",
        headers={"Cache-Control": "private, no-store"},
    )


@app.delete("/api/v1/admin/ambient-events/{event_id}/audio", dependencies=[Depends(verify_admin)])
def delete_ambient_debug_audio(event_id: int) -> dict:
    with connect_db() as db:
        row = db.execute(
            "SELECT debug_raw_file, debug_wav_file FROM ambient_events WHERE id = ?", (event_id,)
        ).fetchone()
        if row is None:
            raise HTTPException(status_code=404, detail="主动互动记录不存在")
        delete_ambient_debug_files(row)
        db.execute(
            "UPDATE ambient_events SET debug_raw_file = '', debug_wav_file = '', debug_wav_bytes = 0 WHERE id = ?",
            (event_id,),
        )
    return {"ok": True, "event_id": event_id}


@app.delete("/api/v1/admin/ambient-events/{event_id}", dependencies=[Depends(verify_admin)])
def delete_ambient_event(event_id: int) -> dict:
    with connect_db() as db:
        event = db.execute(
            "SELECT debug_raw_file, debug_wav_file FROM ambient_events WHERE id = ?", (event_id,)
        ).fetchone()
        if event is None:
            raise HTTPException(status_code=404, detail="主动互动记录不存在")
        speech_rows = db.execute(
            "SELECT file_name FROM speech_assets WHERE ambient_event_id = ?", (event_id,)
        ).fetchall()
        delete_ambient_debug_files(event)
        for row in speech_rows:
            (SPEECH_DIR / row["file_name"]).unlink(missing_ok=True)
        db.execute("DELETE FROM ambient_events WHERE id = ?", (event_id,))
    return {"ok": True, "event_id": event_id}


@app.delete("/api/v1/admin/devices/{device_id}/ambient-debug-audio", dependencies=[Depends(verify_admin)])
def clear_device_ambient_debug_audio(device_id: str) -> dict:
    with connect_db() as db:
        device = db.execute("SELECT device_id FROM devices WHERE device_id = ?", (device_id,)).fetchone()
        if device is None:
            raise HTTPException(status_code=404, detail="Device not found")
        rows = db.execute(
            "SELECT id, debug_raw_file, debug_wav_file FROM ambient_events WHERE device_id = ?",
            (device_id,),
        ).fetchall()
        cleared = 0
        for row in rows:
            if ambient_debug_file_size(row["debug_raw_file"]) or ambient_debug_file_size(row["debug_wav_file"]):
                cleared += 1
            delete_ambient_debug_files(row)
        db.execute(
            "UPDATE ambient_events SET debug_raw_file = '', debug_wav_file = '', debug_wav_bytes = 0 "
            "WHERE device_id = ?",
            (device_id,),
        )
    return {"ok": True, "device_id": device_id, "cleared_events": cleared}


@app.post("/api/v1/admin/firmware", status_code=201, dependencies=[Depends(verify_admin)])
async def upload_firmware(
    firmware: Annotated[UploadFile, File()],
    version: Annotated[str, Form(min_length=1, max_length=64)],
) -> dict:
    content = await firmware.read(MAX_FIRMWARE_BYTES + 1)
    if not content or len(content) > MAX_FIRMWARE_BYTES:
        raise HTTPException(status_code=413, detail="Firmware must be between 1 byte and 5 MB")
    if content[0] != 0xE9:
        raise HTTPException(status_code=415, detail="ESP application binary required")

    digest = hashlib.sha256(content).hexdigest()
    file_name = f"firmware-{uuid.uuid4().hex}.bin"
    final_path = FIRMWARE_DIR / file_name
    temporary_path = FIRMWARE_DIR / f".{file_name}.part"
    temporary_path.write_bytes(content)
    temporary_path.replace(final_path)
    try:
        with connect_db() as db:
            cursor = db.execute(
                "INSERT INTO firmware_releases(version, file_name, byte_size, sha256, download_token, created_at_ms) "
                "VALUES (?, ?, ?, ?, ?, ?)",
                (version.strip(), file_name, len(content), digest, secrets.token_urlsafe(32), now_ms()),
            )
            release_id = cursor.lastrowid
    except Exception:
        final_path.unlink(missing_ok=True)
        raise
    return {"ok": True, "release_id": release_id, "version": version.strip(), "sha256": digest,
            "byte_size": len(content)}


@app.get("/api/v1/device/firmware/{release_id}")
def download_firmware(release_id: int, token: str = Query(min_length=24, max_length=128)) -> FileResponse:
    with connect_db() as db:
        row = db.execute(
            "SELECT file_name, download_token FROM firmware_releases WHERE id = ?", (release_id,)
        ).fetchone()
    if row is None or not secrets.compare_digest(token, row["download_token"]):
        raise HTTPException(status_code=404, detail="Firmware not found")
    path = FIRMWARE_DIR / row["file_name"]
    if not path.is_file():
        raise HTTPException(status_code=404, detail="Firmware file not found")
    return FileResponse(path, media_type="application/octet-stream", filename="xiaozhi.bin")


@app.post("/api/v1/admin/devices/{device_id}/ota", status_code=202, dependencies=[Depends(verify_admin)])
def request_ota(device_id: str, payload: OtaQueueRequest, request: Request) -> dict:
    with connect_db() as db:
        device = db.execute("SELECT last_seen_ms FROM devices WHERE device_id = ?", (device_id,)).fetchone()
        if device is None:
            raise HTTPException(status_code=404, detail="Device not found")
        if now_ms() - device["last_seen_ms"] > ONLINE_WINDOW_SECONDS * 1000:
            raise HTTPException(status_code=409, detail="Device is offline")
        release = db.execute(
            "SELECT id, version, sha256, download_token FROM firmware_releases WHERE id = ?",
            (payload.release_id,),
        ).fetchone()
        if release is None:
            raise HTTPException(status_code=404, detail="Firmware release not found")
        active = db.execute(
            "SELECT id FROM commands WHERE device_id = ? AND command = 'firmware_update' "
            "AND status IN ('queued', 'running')",
            (device_id,),
        ).fetchone()
        if active is not None:
            return {"ok": True, "command_id": active["id"], "reused": True}

        origin = str(request.base_url).rstrip("/")
        if ROOT_PATH and not origin.endswith(ROOT_PATH):
            origin += ROOT_PATH
        url = f"{origin}/api/v1/device/firmware/{release['id']}?token={release['download_token']}"
        if not url.startswith("https://") and request.url.hostname not in {"testserver", "127.0.0.1", "localhost"}:
            raise HTTPException(status_code=409, detail="Public HTTPS proxy configuration required")
        command_payload = json.dumps(
            {"url": url, "version": release["version"], "sha256": release["sha256"]},
            separators=(",", ":"),
        )
        cursor = db.execute(
            "INSERT INTO commands(device_id, command, payload_json, status, requested_at_ms) "
            "VALUES (?, 'firmware_update', ?, 'queued', ?)",
            (device_id, command_payload, now_ms()),
        )
    return {"ok": True, "command_id": cursor.lastrowid, "reused": False,
            "version": release["version"], "sha256": release["sha256"]}


@app.get("/api/v1/admin/chats", dependencies=[Depends(verify_admin)])
def list_chats(device_id: str = "", limit: int = Query(default=100, ge=1, le=500)) -> dict:
    where = "WHERE device_id = ?" if device_id else ""
    params: tuple = (device_id, limit) if device_id else (limit,)
    with connect_db() as db:
        rows = db.execute(
            f"SELECT * FROM chats {where} ORDER BY created_at_ms DESC LIMIT ?", params
        ).fetchall()
    chats = []
    for row in rows:
        item = dict(row)
        item["occurred_at_ms"] = normalize_device_timestamp(item["occurred_at_ms"], item["created_at_ms"])
        chats.append(item)
    return {"ok": True, "chats": chats}


@app.get("/api/v1/admin/photos", dependencies=[Depends(verify_admin)])
def list_photos(device_id: str = "", limit: int = Query(default=60, ge=1, le=200)) -> dict:
    where = "WHERE device_id = ?" if device_id else ""
    params: tuple = (device_id, limit) if device_id else (limit,)
    with connect_db() as db:
        rows = db.execute(
            f"SELECT * FROM photos {where} ORDER BY created_at_ms DESC LIMIT ?", params
        ).fetchall()
    photos = []
    for row in rows:
        item = dict(row)
        item["captured_at_ms"] = normalize_device_timestamp(item["captured_at_ms"], item["created_at_ms"])
        photos.append(item)
    return {"ok": True, "photos": photos}


@app.get("/api/v1/admin/photos/{photo_id}/image", dependencies=[Depends(verify_admin)])
def get_photo_image(photo_id: int) -> FileResponse:
    with connect_db() as db:
        row = db.execute("SELECT file_name, content_type FROM photos WHERE id = ?", (photo_id,)).fetchone()
    if row is None:
        raise HTTPException(status_code=404, detail="Photo not found")
    path = PHOTO_DIR / row["file_name"]
    if not path.is_file():
        raise HTTPException(status_code=404, detail="Photo file not found")
    return FileResponse(path, media_type=row["content_type"])


@app.delete("/api/v1/admin/photos/{photo_id}", dependencies=[Depends(verify_admin)])
def delete_photo(photo_id: int) -> dict:
    with connect_db() as db:
        row = db.execute("SELECT file_name FROM photos WHERE id = ?", (photo_id,)).fetchone()
        if row is None:
            raise HTTPException(status_code=404, detail="Photo not found")
        db.execute("DELETE FROM photos WHERE id = ?", (photo_id,))
    (PHOTO_DIR / row["file_name"]).unlink(missing_ok=True)
    return {"ok": True}


app.mount("/", StaticFiles(directory=BASE_DIR / "static", html=True), name="dashboard")
