import argparse
import json
import os
import sys
import time
import wave

import numpy as np
import sherpa_onnx

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def threads_value(value: str) -> int:
    if value == "auto":
        return max(1, min(4, (os.cpu_count() or 2) // 2))
    try:
        return max(1, int(value))
    except ValueError:
        return 2


def make_recognizer(model_id: str, model_dir: str, threads: str):
    num_threads = threads_value(threads)
    tokens = os.path.join(model_dir, "tokens.txt")

    if model_id == "firered_ctc":
        return sherpa_onnx.OfflineRecognizer.from_fire_red_asr_ctc(
            model=os.path.join(model_dir, "model.int8.onnx"),
            tokens=tokens,
            num_threads=num_threads,
            provider="cpu",
        )

    if model_id == "firered_aed":
        return sherpa_onnx.OfflineRecognizer.from_fire_red_asr(
            encoder=os.path.join(model_dir, "encoder.int8.onnx"),
            decoder=os.path.join(model_dir, "decoder.int8.onnx"),
            tokens=tokens,
            num_threads=num_threads,
            provider="cpu",
        )

    if model_id == "sensevoice":
        return sherpa_onnx.OfflineRecognizer.from_sense_voice(
            model=os.path.join(model_dir, "model.int8.onnx"),
            tokens=tokens,
            num_threads=num_threads,
            use_itn=True,
            provider="cpu",
        )

    raise ValueError(f"Unsupported model_id: {model_id}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-id", required=True)
    parser.add_argument("--model-dir", required=True)
    parser.add_argument("--threads", default="auto")
    parser.add_argument("--wav", required=True)
    args = parser.parse_args()

    started = time.perf_counter()
    recognizer = make_recognizer(args.model_id, args.model_dir, args.threads)
    loaded = time.perf_counter()

    with wave.open(args.wav, "rb") as wav:
        sample_rate = wav.getframerate()
        channels = wav.getnchannels()
        sample_width = wav.getsampwidth()
        frames = wav.readframes(wav.getnframes())

    if sample_width != 2:
        raise ValueError(f"Expected 16-bit PCM wav, got sample width {sample_width}")

    samples = np.frombuffer(frames, dtype=np.int16)
    if channels > 1:
        samples = samples.reshape(-1, channels).mean(axis=1).astype(np.int16)
    samples = samples.astype(np.float32) / 32768.0

    stream = recognizer.create_stream()
    stream.accept_waveform(sample_rate, samples)
    recognizer.decode_stream(stream)
    decoded = time.perf_counter()

    result = stream.result
    text = getattr(result, "text", "") or ""
    payload = {
        "ok": True,
        "text": text.strip(),
        "model_id": args.model_id,
        "load_ms": int((loaded - started) * 1000),
        "decode_ms": int((decoded - loaded) * 1000),
    }
    print(json.dumps(payload, ensure_ascii=False), flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(json.dumps({"ok": False, "error": str(exc)}, ensure_ascii=False), flush=True)
        raise SystemExit(1)
