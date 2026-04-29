import argparse
import json
import os
import socketserver
import sys
import threading
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
            language="",
            use_itn=True,
            provider="cpu",
        )

    raise ValueError(f"Unsupported model_id: {model_id}")


def default_punctuation_model() -> str:
    return os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "models",
        "sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8",
        "model.int8.onnx",
    )


def make_punctuation(model_path: str, threads: str):
    return sherpa_onnx.OfflinePunctuation(
        sherpa_onnx.OfflinePunctuationConfig(
            model=sherpa_onnx.OfflinePunctuationModelConfig(
                ct_transformer=model_path,
                num_threads=threads_value(threads),
                provider="cpu",
            )
        )
    )


def default_vad_model() -> str:
    return os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "models",
        "silero_vad.int8.onnx",
    )


def make_vad_detector(model_path: str, threads: str):
    config = sherpa_onnx.VadModelConfig(
        silero_vad=sherpa_onnx.SileroVadModelConfig(
            model=model_path,
            threshold=0.5,
            min_silence_duration=0.25,
            min_speech_duration=0.25,
            max_speech_duration=30.0,
            window_size=512,
        ),
        sample_rate=16000,
        num_threads=threads_value(threads),
        provider="cpu",
    )
    if not config.validate():
        raise ValueError(f"Invalid Silero VAD config: {model_path}")
    return sherpa_onnx.VoiceActivityDetector(config, buffer_size_in_seconds=600)


def read_wav(path: str):
    with wave.open(path, "rb") as wav:
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
    return sample_rate, samples


class AsrState:
    def __init__(self):
        self.lock = threading.Lock()
        self.key = None
        self.recognizer = None

    def clear(self):
        with self.lock:
            self.key = None
            self.recognizer = None

    def recognize(
        self,
        model_id: str,
        model_dir: str,
        threads: str,
        wav_path: str,
        enable_vad: bool = True,
    ):
        key = (model_id, os.path.abspath(model_dir), threads)
        load_ms = 0
        sample_rate, samples = read_wav(wav_path)
        vad_info = VAD.trim(sample_rate, samples, threads) if enable_vad else {
            "vad_enabled": False,
            "samples": samples,
            "vad_ms": 0,
            "vad_segments": 0,
            "speech_ms": int(len(samples) * 1000 / sample_rate) if sample_rate else 0,
            "warning": "",
        }
        samples = vad_info.pop("samples")
        if samples.size == 0:
            return {
                "ok": True,
                "text": "",
                "model_id": model_id,
                "loaded": False,
                "load_ms": 0,
                "decode_ms": 0,
                **vad_info,
            }

        with self.lock:
            if self.key != key or self.recognizer is None:
                started = time.perf_counter()
                self.recognizer = make_recognizer(model_id, model_dir, threads)
                self.key = key
                load_ms = int((time.perf_counter() - started) * 1000)

            started = time.perf_counter()
            stream = self.recognizer.create_stream()
            stream.accept_waveform(sample_rate, samples)
            self.recognizer.decode_stream(stream)
            decode_ms = int((time.perf_counter() - started) * 1000)
            text = (getattr(stream.result, "text", "") or "").strip()
            if text in {"<sil>", "<blk>"}:
                text = ""

        return {
            "ok": True,
            "text": text,
            "model_id": model_id,
            "loaded": load_ms > 0,
            "load_ms": load_ms,
            "decode_ms": decode_ms,
            **vad_info,
        }


class VadState:
    def __init__(self):
        self.lock = threading.Lock()
        self.key = None
        self.detector = None

    def clear(self):
        with self.lock:
            self.key = None
            self.detector = None

    def trim(self, sample_rate: int, samples, threads: str, model_path: str = ""):
        started = time.perf_counter()
        original_ms = int(len(samples) * 1000 / sample_rate) if sample_rate else 0
        if sample_rate != 16000:
            return {
                "vad_enabled": True,
                "samples": samples,
                "vad_ms": 0,
                "vad_segments": 0,
                "speech_ms": original_ms,
                "warning": f"VAD skipped: expected 16000 Hz, got {sample_rate}",
            }

        model_path = model_path or default_vad_model()
        if not os.path.exists(model_path):
            return {
                "vad_enabled": True,
                "samples": samples,
                "vad_ms": 0,
                "vad_segments": 0,
                "speech_ms": original_ms,
                "warning": f"VAD model not found: {model_path}",
            }

        key = (os.path.abspath(model_path), threads)
        with self.lock:
            if self.key != key or self.detector is None:
                self.detector = make_vad_detector(model_path, threads)
                self.key = key
            self.detector.reset()
            window_size = int(self.detector.config.silero_vad.window_size)
            for start in range(0, len(samples), window_size):
                self.detector.accept_waveform(samples[start : start + window_size])
            self.detector.flush()

            ranges = []
            pad = int(0.28 * sample_rate)
            while not self.detector.empty():
                segment = self.detector.front
                start = max(0, int(segment.start) - pad)
                end = min(len(samples), int(segment.start) + len(segment.samples) + pad)
                if end > start:
                    if ranges and start <= ranges[-1][1]:
                        ranges[-1] = (ranges[-1][0], max(ranges[-1][1], end))
                    else:
                        ranges.append((start, end))
                self.detector.pop()

        if not ranges:
            rms = float(np.sqrt(np.mean(np.square(samples)))) if len(samples) else 0.0
            peak = float(np.max(np.abs(samples))) if len(samples) else 0.0
            if original_ms >= 2000 and (rms > 0.008 or peak > 0.08):
                trimmed = samples
                warning = "VAD fallback: no segment detected but audio is not silent"
            else:
                trimmed = np.array([], dtype=np.float32)
                warning = ""
        else:
            start = ranges[0][0]
            end = ranges[-1][1]
            trimmed = samples[start:end].astype(np.float32, copy=False)
            detected_ms = sum(end - start for start, end in ranges) * 1000 / sample_rate
            trimmed_ms = len(trimmed) * 1000 / sample_rate
            if original_ms >= 2500 and detected_ms < min(1200, original_ms * 0.25):
                trimmed = samples
                warning = (
                    "VAD fallback: detected speech was too short "
                    f"({int(detected_ms)} ms of {original_ms} ms)"
                )
            else:
                warning = ""

        vad_ms = int((time.perf_counter() - started) * 1000)
        return {
            "vad_enabled": True,
            "samples": trimmed,
            "vad_ms": vad_ms,
            "vad_segments": len(ranges),
            "speech_ms": int(len(trimmed) * 1000 / sample_rate) if sample_rate else 0,
            "warning": warning,
        }


class PunctuationState:
    def __init__(self):
        self.lock = threading.Lock()
        self.key = None
        self.punctuation = None

    def clear(self):
        with self.lock:
            self.key = None
            self.punctuation = None

    def add_punctuation(self, text: str, threads: str, model_path: str = ""):
        if not text:
            return text, 0, False, ""

        model_path = model_path or default_punctuation_model()
        if not os.path.exists(model_path):
            return text, 0, False, f"punctuation model not found: {model_path}"

        key = (os.path.abspath(model_path), threads)
        loaded = False
        with self.lock:
            if self.key != key or self.punctuation is None:
                self.punctuation = make_punctuation(model_path, threads)
                self.key = key
                loaded = True

            started = time.perf_counter()
            text = self.punctuation.add_punctuation(text).strip()
            punct_ms = int((time.perf_counter() - started) * 1000)

        return text, punct_ms, loaded, ""


STATE = AsrState()
VAD = VadState()
PUNCT = PunctuationState()


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        raw = self.rfile.readline(1024 * 1024)
        if not raw:
            return
        try:
            request = json.loads(raw.decode("utf-8"))
            cmd = request.get("cmd")
            if cmd == "ping":
                response = {"ok": True, "status": "ready"}
            elif cmd == "reload":
                STATE.clear()
                VAD.clear()
                PUNCT.clear()
                response = {"ok": True, "status": "reloaded"}
            elif cmd == "shutdown":
                response = {"ok": True, "status": "shutting_down"}
                self.wfile.write((json.dumps(response, ensure_ascii=False) + "\n").encode("utf-8"))
                self.wfile.flush()
                threading.Thread(target=self.server.shutdown, daemon=True).start()
                return
            elif cmd == "recognize":
                response = STATE.recognize(
                    request["model_id"],
                    request["model_dir"],
                    request.get("threads", "auto"),
                    request["wav"],
                    bool(request.get("enable_vad", True)),
                )
                postprocess = request.get("postprocess", "itn")
                if response.get("ok") and postprocess in {"itn", "punct", "llm"}:
                    text, punct_ms, punct_loaded, warning = PUNCT.add_punctuation(
                        response.get("text", ""),
                        request.get("threads", "auto"),
                        request.get("punctuation_model", ""),
                    )
                    response["text"] = text
                    response["postprocess"] = "punctuation"
                    response["punct_ms"] = punct_ms
                    response["punct_loaded"] = punct_loaded
                    if warning:
                        response["warning"] = warning
                else:
                    response["postprocess"] = "none"
            else:
                response = {"ok": False, "error": f"Unsupported cmd: {cmd}"}
        except Exception as exc:
            response = {"ok": False, "error": str(exc)}

        self.wfile.write((json.dumps(response, ensure_ascii=False) + "\n").encode("utf-8"))
        self.wfile.flush()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18088)
    args = parser.parse_args()

    with Server((args.host, args.port), Handler) as server:
        server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
