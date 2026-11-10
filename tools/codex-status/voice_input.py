"""Local speech recognition and follow-up delivery for the office mode."""
import asyncio
import re
import sys
import tempfile
import threading
import wave
from pathlib import Path

MAX_PCM_BYTES = 6 * 16000 * 2
THREAD_ID = re.compile(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}")


async def run_command(*args, timeout):
    proc = await asyncio.create_subprocess_exec(
        *args, stdin=asyncio.subprocess.DEVNULL, stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.DEVNULL,
    )
    try:
        output, _ = await asyncio.wait_for(proc.communicate(), timeout)
    finally:
        if proc.returncode is None:
            proc.kill()
            await proc.wait()
    if proc.returncode:
        raise RuntimeError("command failed")
    return output.decode("utf-8")


class VoiceInput:
    def __init__(self, key, whisper, model, codex):
        self.key = key
        self.whisper = whisper
        self.model = model
        self.codex = codex

    async def transcribe(self, pcm, *, choice=False):
        with tempfile.TemporaryDirectory(prefix="ls26-speech-") as directory:
            filename = Path(directory) / "command.wav"
            with wave.open(str(filename), "wb") as wav:
                wav.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
                wav.writeframes(pcm)
            prompt = "我选第一项、第二项、第三项。" if choice else ""
            prompt_args = ("--prompt", prompt) if prompt else ()
            result = await run_command(
                self.whisper, "-m", self.model, "-l", "zh", "-nt", "-np",
                *prompt_args, "-f", str(filename), timeout=15,
            )
        return clean_transcript(result, choice=choice, prompt=prompt)

    async def submit(self, thread, text):
        if not THREAD_ID.fullmatch(thread):
            raise ValueError("invalid thread id")
        # The process that already owns the conversation consumes the queue.
        # Avoid --resume so the bridge never loads a competing copy.
        await run_command(self.codex, "queue", "--thread", thread, "--message", text, timeout=4)

def clean_transcript(text, *, choice=False, prompt=""):
    result = " ".join(text.split())
    if prompt and result.rstrip("。！？.!?") == prompt.rstrip("。！？.!?"):
        return ""
    if not choice:
        compact = "".join(char.lower() for char in result if char.isalnum())
        if re.fullmatch(r"(?:.{0,2})(.)\1{4,}", compact) or re.fullmatch(r"(.{2,12})\1{2,}", compact):
            return ""
    return result if 0 < len(result) <= 500 and any(c.isalnum() for c in result) else ""


class SenseVoiceInput(VoiceInput):
    def __init__(self, key, directory, codex):
        self.key = key
        self.codex = codex
        sys.path.append(str(directory / "deps"))
        import numpy as np
        import sherpa_onnx

        self.numpy = np
        self.lock = threading.Lock()
        self.recognizer = sherpa_onnx.OfflineRecognizer.from_sense_voice(
            model=str(directory / "model.int8.onnx"),
            tokens=str(directory / "tokens.txt"),
            num_threads=4, language="zh", use_itn=True,
        )

    def decode(self, pcm):
        samples = self.numpy.frombuffer(pcm, dtype="<i2").astype(self.numpy.float32)
        # Ignore silence and very low-level input before the model can hallucinate text.
        frames = samples[:samples.size // 320 * 320].reshape(-1, 320)
        voiced = self.numpy.mean(self.numpy.abs(frames), axis=1) >= 80
        if self.numpy.count_nonzero(voiced) < 6:
            return ""
        samples /= 32768
        with self.lock:
            stream = self.recognizer.create_stream()
            stream.accept_waveform(16000, samples)
            self.recognizer.decode_stream(stream)
            return stream.result.text

    async def transcribe(self, pcm, *, choice=False):
        result = await asyncio.to_thread(self.decode, pcm)
        return clean_transcript(result, choice=choice)
