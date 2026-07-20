from __future__ import annotations

import array
import math
import struct
import sys
import wave
from pathlib import Path


MAGIC = b"CAP1"
HEADER = struct.Struct("<4sHHH")
MAX_PACKET_BYTES = 4096
MAX_FRAMES = 200


class AmbientAudioError(ValueError):
    pass


def inspect_cat_opus(content: bytes) -> tuple[int, int, list[bytes]]:
    if len(content) < HEADER.size:
        raise AmbientAudioError("ambient audio header is missing")
    magic, sample_rate, frame_duration_ms, frame_count = HEADER.unpack_from(content)
    if magic != MAGIC:
        raise AmbientAudioError("ambient audio magic is invalid")
    if sample_rate != 16000 or frame_duration_ms not in {20, 40, 60}:
        raise AmbientAudioError("ambient audio format is unsupported")
    if frame_count == 0 or frame_count > MAX_FRAMES:
        raise AmbientAudioError("ambient audio frame count is invalid")

    packets: list[bytes] = []
    offset = HEADER.size
    for _ in range(frame_count):
        if offset + 2 > len(content):
            raise AmbientAudioError("ambient audio packet length is missing")
        packet_size = struct.unpack_from("<H", content, offset)[0]
        offset += 2
        if packet_size == 0 or packet_size > MAX_PACKET_BYTES or offset + packet_size > len(content):
            raise AmbientAudioError("ambient audio packet is invalid")
        packets.append(content[offset:offset + packet_size])
        offset += packet_size
    if offset != len(content):
        raise AmbientAudioError("ambient audio has trailing bytes")
    return sample_rate, frame_duration_ms, packets


def decode_cat_opus(content: bytes) -> tuple[bytes, int, int]:
    sample_rate, frame_duration_ms, packets = inspect_cat_opus(content)
    try:
        import opuslib
    except Exception as exc:  # pragma: no cover - deployment dependency
        raise RuntimeError("Opus decoder is unavailable") from exc

    decoder = opuslib.Decoder(sample_rate, 1)
    frame_samples = sample_rate * frame_duration_ms // 1000
    pcm = bytearray()
    for packet in packets:
        try:
            pcm.extend(decoder.decode(packet, frame_samples, False))
        except Exception as exc:
            raise AmbientAudioError("ambient Opus packet cannot be decoded") from exc
    return bytes(pcm), sample_rate, len(packets) * frame_duration_ms


def pcm_diagnostics(pcm: bytes) -> dict[str, float | int]:
    if not pcm or len(pcm) % 2:
        raise AmbientAudioError("ambient PCM data is invalid")
    samples = array.array("h")
    samples.frombytes(pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    peak = max(abs(sample) for sample in samples)
    square_sum = sum(sample * sample for sample in samples)
    rms = math.sqrt(square_sum / len(samples))

    def dbfs(value: float) -> float:
        return round(max(-96.0, 20.0 * math.log10(value / 32768.0)), 2) if value else -96.0

    return {
        "sample_count": len(samples),
        "pcm_bytes": len(pcm),
        "peak_sample": peak,
        "rms_sample": round(rms, 2),
        "peak_dbfs": dbfs(float(peak)),
        "rms_dbfs": dbfs(rms),
    }


def write_pcm_wav(path: Path, pcm: bytes, sample_rate: int) -> None:
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        output.writeframes(pcm)
