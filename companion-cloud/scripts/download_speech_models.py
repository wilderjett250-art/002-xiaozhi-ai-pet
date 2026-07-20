from __future__ import annotations

import argparse
import shutil
import tarfile
import tempfile
import urllib.request
from pathlib import Path


MODELS = (
    (
        "sense-voice",
        "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/"
        "sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17.tar.bz2",
        "sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17",
        ("model.int8.onnx", "tokens.txt"),
    ),
    (
        "vits-zh-ll",
        "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/"
        "sherpa-onnx-vits-zh-ll.tar.bz2",
        "sherpa-onnx-vits-zh-ll",
        ("model.onnx", "tokens.txt", "lexicon.txt", "phone.fst", "number.fst", "date.fst"),
    ),
)


def safe_extract(archive: tarfile.TarFile, destination: Path) -> None:
    root = destination.resolve()
    for member in archive.getmembers():
        target = (destination / member.name).resolve()
        if root != target and root not in target.parents:
            raise RuntimeError(f"unsafe archive member: {member.name}")
    archive.extractall(destination)


def complete(path: Path, files: tuple[str, ...]) -> bool:
    return all((path / name).is_file() and (path / name).stat().st_size > 0 for name in files)


def download_model(target_root: Path, name: str, url: str, source_dir: str, files: tuple[str, ...]) -> None:
    target = target_root / name
    if complete(target, files):
        print(f"{name}: ready")
        return
    target_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f"{name}-") as temporary:
        temporary_path = Path(temporary)
        archive_path = temporary_path / "model.tar.bz2"
        print(f"{name}: downloading {url}")
        urllib.request.urlretrieve(url, archive_path)
        with tarfile.open(archive_path, "r:bz2") as archive:
            safe_extract(archive, temporary_path)
        extracted = temporary_path / source_dir
        if not complete(extracted, files):
            raise RuntimeError(f"{name}: downloaded model is incomplete")
        if target.exists():
            shutil.rmtree(target)
        shutil.move(str(extracted), str(target))
    print(f"{name}: installed at {target}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Download offline ASR and TTS models for Companion Cloud")
    parser.add_argument("--target", type=Path, default=Path("/app/models"))
    args = parser.parse_args()
    for model in MODELS:
        download_model(args.target, *model)


if __name__ == "__main__":
    main()
