"""Launch the assistant-managed bridge with optional local speech recognition."""
import os
import shutil
import sys
from pathlib import Path

def find_codex(data):
    configured = data / "codex-command"
    candidates = [configured.read_text().strip()] if configured.is_file() else []
    candidates.append(shutil.which("codex"))
    for root in (Path("/Applications"), Path.home() / "Applications"):
        for app in ("ChatGPT.app", "Codex.app"):
            resources = root / app / "Contents/Resources"
            candidates.extend((resources / "codex",
                               resources / "codex-cli/CodexCLI.app/Contents/MacOS/codex"))
    return next((str(path) for path in candidates
                 if path and Path(path).is_file() and os.access(path, os.X_OK)), None)


data = Path(__file__).resolve().parent
os.environ["CODEX_STATUS_TOKEN"] = (data / "status-token").read_text().strip()
args = [str(data / "bridge.py"), "--bind", "0.0.0.0", "--discover", "--agent-voice"]
key, whisper, model = (data / name for name in ("voice-key", "whisper-cli", "ggml-small.bin"))
codex = find_codex(data)
sensevoice = data / "SenseVoice"
if codex and key.is_file() and all((sensevoice / name).exists()
                                  for name in ("model.int8.onnx", "tokens.txt", "deps")):
    args += ["--voice-key-file", str(key), "--sensevoice-dir", str(sensevoice), "--codex", codex]
elif codex and all(path.is_file() for path in (key, whisper, model)):
    args += ["--voice-key-file", str(key), "--whisper", str(whisper),
             "--whisper-model", str(model), "--codex", codex]
os.execv(sys.executable, [sys.executable, *args])
