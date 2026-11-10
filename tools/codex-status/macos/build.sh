#!/bin/bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: bash $0 <output directory>" >&2
    exit 2
fi
source_dir=$(cd "$(dirname "$0")" && pwd)
app="$1/摩突突助手.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$source_dir/Info.plist" "$app/Contents/Info.plist"
bridge_resources="$app/Contents/Resources/Bridge"
mkdir -p "$bridge_resources"
for file in bridge.py choice.py discovery.py speech.py voice_input.py hook.py hooks.example.json requirements.txt; do
    cp "$source_dir/../$file" "$bridge_resources/"
done
cp "$source_dir/setup_bridge.py" "$source_dir/launch-managed-bridge.py" "$bridge_resources/"
if [[ -n "${MOTUTU_DEVICE_PROFILE:-}" ]]; then
    install -m 644 "$MOTUTU_DEVICE_PROFILE" "$bridge_resources/device-profile.json"
fi
if [[ -n "${MOTUTU_BUILD_PYTHON:-}" ]]; then
    python="$MOTUTU_BUILD_PYTHON"
    prefix=$("$python" -c 'import sys; print(sys.base_prefix)')
    version=$("$python" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
    runtime="$bridge_resources/Python"
    mkdir -p "$runtime/bin" "$runtime/lib/python$version"
    cp -L "$prefix/bin/python$version" "$runtime/bin/python3"
    rsync -a --exclude site-packages --exclude __pycache__ \
        "$prefix/lib/python$version/" "$runtime/lib/python$version/"
    find "$prefix/lib" -maxdepth 1 -name '*.dylib' -exec cp -L {} "$runtime/lib/" \;
    "$python" -m pip install --disable-pip-version-check --only-binary=:all: \
        --target "$runtime/lib/python$version/site-packages" -r "$source_dir/../requirements.txt" >&2
    "$runtime/bin/python3" -I -B -c 'import aiohttp, imageio_ffmpeg, ssl'
fi
if [[ -n "${MOTUTU_SENSEVOICE_ASSETS:-}" ]]; then
    voice_resources="$bridge_resources/SenseVoice"
    mkdir -p "$voice_resources"
    for file in model.int8.onnx tokens.txt LICENSE; do
        install -m 644 "$MOTUTU_SENSEVOICE_ASSETS/$file" "$voice_resources/$file"
    done
    ditto "$MOTUTU_SENSEVOICE_ASSETS/deps" "$voice_resources/deps"
fi
if [[ -n "${MOTUTU_WHISPER_ASSETS:-}" ]]; then
    whisper_binary="$MOTUTU_WHISPER_ASSETS/whisper-cli"
    xcrun lipo -verify_arch "$(uname -m)" "$whisper_binary"
    if ! xcrun vtool -arch "$(uname -m)" -show-build "$whisper_binary" | \
        awk '/minos/ { split($2, v, "."); found = 1; if (v[1] > 13 || (v[1] == 13 && (v[2] > 0 || v[3] > 0))) too_new = 1 } END { exit(!found || too_new) }'; then
        echo "Whisper must be built with CMAKE_OSX_DEPLOYMENT_TARGET=13.0 or earlier" >&2
        exit 1
    fi
    voice_resources="$bridge_resources/Whisper"
    mkdir -p "$voice_resources"
    install -m 755 "$MOTUTU_WHISPER_ASSETS/whisper-cli" "$voice_resources/whisper-cli"
    install -m 644 "$MOTUTU_WHISPER_ASSETS/ggml-small.bin" "$voice_resources/ggml-small.bin"
    install -m 644 "$MOTUTU_WHISPER_ASSETS/LICENSE" "$voice_resources/LICENSE"
fi
icon_tmp=$(mktemp -d)
trap 'rm -R -- "$icon_tmp"' EXIT
iconset="$icon_tmp/AppIcon.iconset"
mkdir "$iconset"
for size in 16 32 128 256 512; do
    sips -z "$size" "$size" "$source_dir/AppIcon.png" --out "$iconset/icon_${size}x${size}.png" >/dev/null
    doubled=$((size * 2))
    sips -z "$doubled" "$doubled" "$source_dir/AppIcon.png" --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$app/Contents/Resources/AppIcon.icns"
SDKROOT=$(xcrun --show-sdk-path) \
    xcrun swiftc -target "$(uname -m)-apple-macosx13.0" -O -parse-as-library "$source_dir/LS26Assistant.swift" \
    -o "$app/Contents/MacOS/LS26Assistant"
printf '%s\n' "$app"
