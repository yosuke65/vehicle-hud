#!/usr/bin/env bash
# Builds libhudgfx.so for arm64-v8a and copies it into the Unity project,
# where Unity picks up the CPU architecture from the folder name.
set -euo pipefail

NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/28.2.13676358}"
UNITY_PLUGIN_API="${UNITY_PLUGIN_API:-/Applications/Unity/Hub/Editor/6000.5.9f1/Unity.app/Contents/Resources/PluginAPI}"
UNITY_PROJECT="${UNITY_PROJECT:-$HOME/Desktop/development/vehicle-hud-unity}"

HERE="$(cd "$(dirname "$0")" && pwd)"
CXX="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang++"
OUT_DIR="$HERE/build/arm64-v8a"
DEST_DIR="$UNITY_PROJECT/Assets/Plugins/Android/libs/arm64-v8a"

mkdir -p "$OUT_DIR" "$DEST_DIR"
"$CXX" -shared -fPIC -O2 -std=c++17 -Wall -Wextra \
    -I "$UNITY_PLUGIN_API" \
    -static-libstdc++ \
    -o "$OUT_DIR/libhudgfx.so" "$HERE/hudgfx.cpp" \
    -llog
cp "$OUT_DIR/libhudgfx.so" "$DEST_DIR/libhudgfx.so"
echo "Built $OUT_DIR/libhudgfx.so -> $DEST_DIR"
