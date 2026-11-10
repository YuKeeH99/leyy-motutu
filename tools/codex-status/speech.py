"""Render a short reply locally on macOS."""
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def plain_speech_text(text):
    text = re.sub(r"(?ms)^\s*```[^\n]*\n.*?(?:^```[^\n]*$|\Z)", "", text)
    text = re.sub(r"!?\[([^\]\n]+)\]\([^)\n]+\)", r"\1", text)
    text = re.sub(r"https?://\S+", "", text)
    text = re.sub(r"(?m)^\s*(?:[#>*+-]+|\d+[.)])\s+", "", text)
    return " ".join(text.replace("`", "").replace("**", "").split())


def spoken_text(text):
    text = plain_speech_text(text)
    if len(text) <= 120:
        return text
    sentences = re.split(r"(?<=[。！？])\s*|(?<=[.!?])\s+", text, maxsplit=2)
    return "".join(sentences[:2])[:120].rstrip(" ,，、") + "。"


def render_reply(reply):
    if sys.platform != "darwin":
        return None
    text = spoken_text(reply)
    if not text:
        return None
    try:
        import imageio_ffmpeg

        with tempfile.TemporaryDirectory(prefix="codex-speech-") as temp:
            source = Path(temp) / "speech.aiff"
            voice = ["-v", "Tingting"] if re.search(r"[\u3400-\u9fff]", text) else []
            subprocess.run(
                ["/usr/bin/say", *voice, "-r", "185", "-o", str(source), "-f", "-"],
                input=text.encode(), capture_output=True, check=True, timeout=4,
            )
            audio = subprocess.run(
                [imageio_ffmpeg.get_ffmpeg_exe(), "-v", "error", "-i", str(source),
                 "-t", "18", "-ar", "16000", "-ac", "1", "-b:a", "32k",
                 "-write_xing", "0", "-f", "mp3", "pipe:1"],
                capture_output=True, check=True, timeout=3,
            ).stdout
            return audio or None
    except (ImportError, OSError, RuntimeError, subprocess.SubprocessError):
        return None


def summary_context(reply, limit=420):
    """Keep both the task opening and its final caveats within the board frame."""
    data = plain_speech_text(reply).encode("utf-8")
    if len(data) <= limit:
        return data.decode("utf-8")
    separator = " … ".encode("utf-8")
    head = limit // 3
    tail = limit - head - len(separator)
    return (data[:head].decode("utf-8", errors="ignore").rstrip()
            + separator.decode("utf-8")
            + data[-tail:].decode("utf-8", errors="ignore").lstrip())
