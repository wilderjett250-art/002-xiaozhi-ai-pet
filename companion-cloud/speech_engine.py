from __future__ import annotations

import os
import threading
import wave
from pathlib import Path


class SpeechEngine:
    def __init__(self) -> None:
        model_root = Path(os.getenv("PET_CLOUD_SPEECH_MODEL_DIR", "/app/models")).resolve()
        asr_root = model_root / "sense-voice"
        tts_root = model_root / "vits-zh-ll"
        self.asr_model = Path(os.getenv("PET_CLOUD_ASR_MODEL", asr_root / "model.int8.onnx"))
        self.asr_tokens = Path(os.getenv("PET_CLOUD_ASR_TOKENS", asr_root / "tokens.txt"))
        self.tts_model = Path(os.getenv("PET_CLOUD_TTS_MODEL", tts_root / "model.onnx"))
        self.tts_tokens = Path(os.getenv("PET_CLOUD_TTS_TOKENS", tts_root / "tokens.txt"))
        self.tts_lexicon = Path(os.getenv("PET_CLOUD_TTS_LEXICON", tts_root / "lexicon.txt"))
        default_fsts = ",".join(str(tts_root / name) for name in ("phone.fst", "number.fst", "date.fst"))
        self.tts_rule_fsts = os.getenv("PET_CLOUD_TTS_RULE_FSTS", default_fsts)
        self.provider = os.getenv("PET_CLOUD_SPEECH_PROVIDER", "cpu").strip() or "cpu"
        self.num_threads = max(1, min(8, int(os.getenv("PET_CLOUD_SPEECH_THREADS", "2"))))
        self.tts_sid = max(0, int(os.getenv("PET_CLOUD_TTS_SID", "1")))
        self.tts_speed = max(0.75, min(1.35, float(os.getenv("PET_CLOUD_TTS_SPEED", "1.08"))))
        self._recognizer = None
        self._tts = None
        self._lock = threading.Lock()

    def status(self) -> dict:
        dependency_ready = True
        try:
            import sherpa_onnx  # noqa: F401
        except Exception:
            dependency_ready = False
        asr_ready = dependency_ready and self.asr_model.is_file() and self.asr_tokens.is_file()
        tts_ready = (
            dependency_ready
            and self.tts_model.is_file()
            and self.tts_tokens.is_file()
            and self.tts_lexicon.is_file()
        )
        return {
            "ready": asr_ready and tts_ready,
            "asr_ready": asr_ready,
            "tts_ready": tts_ready,
            "provider": self.provider,
            "engine": "sherpa-onnx",
        }

    def _get_recognizer(self):
        if self._recognizer is None:
            import sherpa_onnx

            self._recognizer = sherpa_onnx.OfflineRecognizer.from_sense_voice(
                model=str(self.asr_model),
                tokens=str(self.asr_tokens),
                num_threads=self.num_threads,
                provider=self.provider,
                language="zh",
                use_itn=True,
            )
        return self._recognizer

    def _get_tts(self):
        if self._tts is None:
            import sherpa_onnx

            vits = sherpa_onnx.OfflineTtsVitsModelConfig(
                model=str(self.tts_model),
                lexicon=str(self.tts_lexicon),
                tokens=str(self.tts_tokens),
                length_scale=1.0,
            )
            model = sherpa_onnx.OfflineTtsModelConfig(
                vits=vits,
                num_threads=self.num_threads,
                provider=self.provider,
            )
            config = sherpa_onnx.OfflineTtsConfig(
                model=model,
                rule_fsts=self.tts_rule_fsts,
                max_num_sentences=2,
                silence_scale=0.15,
            )
            if not config.validate():
                raise RuntimeError("sherpa-onnx TTS configuration is invalid")
            self._tts = sherpa_onnx.OfflineTts(config)
        return self._tts

    def transcribe(self, pcm_s16le: bytes, sample_rate: int) -> str:
        if not self.status()["asr_ready"]:
            raise RuntimeError("ASR model is not ready")
        import numpy as np

        samples = np.frombuffer(pcm_s16le, dtype="<i2").astype(np.float32) / 32768.0
        with self._lock:
            recognizer = self._get_recognizer()
            stream = recognizer.create_stream()
            stream.accept_waveform(sample_rate, samples)
            recognizer.decode_stream(stream)
            return stream.result.text.strip()

    def synthesize(self, text: str, target: Path) -> tuple[int, int]:
        if not self.status()["tts_ready"]:
            raise RuntimeError("TTS model is not ready")
        import numpy as np

        with self._lock:
            audio = self._get_tts().generate(
                text,
                sid=min(self.tts_sid, max(0, self._tts.num_speakers - 1)),
                speed=self.tts_speed,
            )
            samples = np.asarray(audio.samples, dtype=np.float32)
            sample_rate = int(audio.sample_rate)
        if samples.size == 0:
            raise RuntimeError("TTS generated no audio")
        pcm = (np.clip(samples, -1.0, 1.0) * 32767.0).astype("<i2").tobytes()
        target.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(target), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(sample_rate)
            output.writeframes(pcm)
        return sample_rate, len(samples)


speech_engine = SpeechEngine()
