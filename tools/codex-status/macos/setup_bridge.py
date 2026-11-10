"""Set up a new Mac from a device-specific pairing file."""
import json
import os
import plistlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

LABEL = "local.ls26.codex-bridge"
SOURCES = ("bridge.py", "choice.py", "discovery.py", "speech.py", "voice_input.py",
           "hook.py", "hooks.example.json", "requirements.txt")


def atomic_write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(content)
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def load_pairing(path):
    data = json.loads(path.read_text())
    if not isinstance(data, dict) or not re.fullmatch(r"[a-zA-Z0-9_-]{16,128}", data.get("status_token", "")):
        raise ValueError("配对文件无效。")
    key = data.get("voice_key", "")
    if key and not re.fullmatch(r"[0-9a-fA-F]{64}", key):
        raise ValueError("配对文件的语音密钥无效。")
    return data["status_token"], key


def install_hooks(home, data, python=None):
    directory = Path(os.environ.get("CODEX_HOME") or home / ".codex")
    target = directory / "hooks.json"
    original = target.read_bytes() if target.exists() else b""
    config = json.loads(original) if original else {}
    hooks = config.setdefault("hooks", {})
    template = json.loads((data / "hooks.example.json").read_text())
    command = " ".join(map(shlex.quote, (str(python or data / "venv/bin/python"), str(data / "hook.py"))))
    changed = False
    for event in template["hooks"]:
        entries = hooks.setdefault(event, [])
        if any(any(handler.get("command") == command for handler in group.get("hooks", []))
               for group in entries):
            continue
        entries.append({"hooks": [{"type": "command", "command": command, "timeout": 2}]})
        changed = True
    if changed:
        backup = directory / "hooks.pre-motutu.json"
        if original and not backup.exists():
            atomic_write(backup, original)
        atomic_write(target, (json.dumps(config, ensure_ascii=False, indent=2) + "\n").encode())


def setup(pairing, resources, home):
    token, voice_key = load_pairing(pairing)
    data = home / "Library/Application Support/LS26 Codex"
    agent = home / "Library/LaunchAgents" / (LABEL + ".plist")
    if agent.exists():
        raise ValueError("此 Mac 已配置桥接，请先从菜单启动现有服务。")
    data.mkdir(mode=0o700, parents=True, exist_ok=True)
    existing = data / "status-token"
    if existing.exists() and existing.read_text().strip() != token:
        raise ValueError("此 Mac 已保留另一台设备的配对配置。")
    for name in SOURCES:
        shutil.copyfile(resources / name, data / name)
    bundled = resources / "Python"
    if (bundled / "bin/python3").is_file():
        runtime = data / "Python"
        shutil.copytree(bundled, runtime, dirs_exist_ok=True, symlinks=True)
        python = runtime / "bin/python3"
        subprocess.run([str(python), "-I", "-B", "-c", "import aiohttp, ssl"], check=True)
    else:
        subprocess.run([sys.executable, "-m", "venv", str(data / "venv")], check=True)
        python = data / "venv/bin/python"
        subprocess.run([str(python), "-m", "pip", "install", "-r", str(data / "requirements.txt")],
                       check=True, stdout=subprocess.DEVNULL)
    voice_assets = resources / "Whisper"
    sensevoice = resources / "SenseVoice"
    if voice_key and sensevoice.is_dir():
        shutil.copytree(sensevoice, data / "SenseVoice", dirs_exist_ok=True)
    elif voice_key and voice_assets.is_dir():
        for name in ("whisper-cli", "ggml-small.bin"):
            shutil.copy2(voice_assets / name, data / name)
    install_hooks(home, data, python)
    atomic_write(existing, token.encode())
    if voice_key:
        atomic_write(data / "voice-key", voice_key.encode())
    launcher = data / "launch-managed-bridge.py"
    shutil.copyfile(resources / launcher.name, launcher)
    config = {
        "Label": LABEL, "ProgramArguments": [str(python), str(launcher)],
        "RunAtLoad": True, "KeepAlive": True, "ThrottleInterval": 10,
        "StandardOutPath": str(data / "bridge.log"), "StandardErrorPath": str(data / "bridge.log"),
    }
    atomic_write(agent, plistlib.dumps(config))


def upgrade_sensevoice(resources, home):
    data = home / "Library/Application Support/LS26 Codex"
    python = data / "Python/bin/python3"
    launcher = data / "launch-managed-bridge.py"
    if not python.is_file() or not launcher.is_file():
        raise ValueError("当前桥接安装方式需要手动升级。")
    with tempfile.TemporaryDirectory(dir=data) as temporary:
        staged = Path(temporary) / "SenseVoice"
        shutil.copytree(resources / "SenseVoice", staged)
        subprocess.run([str(python), "-I", "-B", "-c",
                        "import sys; sys.path.insert(0, sys.argv[1]); import numpy, sherpa_onnx",
                        str(staged / "deps")], check=True)
        for name in ("voice_input.py", "bridge.py", launcher.name):
            atomic_write(data / name, (resources / name).read_bytes())
        staged.rename(data / "SenseVoice")


if __name__ == "__main__":
    try:
        resources = Path(__file__).parent
        if sys.argv[1] == "--upgrade-sensevoice":
            upgrade_sensevoice(resources, Path.home())
        else:
            setup(Path(sys.argv[1]), resources, Path.home())
    except subprocess.CalledProcessError:
        print("桥接环境安装失败。请检查 Python 安装和网络连接后重试。")
        sys.exit(1)
    except (ValueError, TypeError, OSError):
        print("无法完成初始化。请检查配对文件和本机 Codex 配置。")
        sys.exit(1)
