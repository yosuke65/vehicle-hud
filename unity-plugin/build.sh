#!/usr/bin/env bash
# Builds libhudgfx.so (arm64-v8a by default) and copies it into the Unity project,
# where Unity picks up the CPU architecture from the folder name.
set -euo pipefail

NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/28.2.13676358}"
UNITY_PLUGIN_API="${UNITY_PLUGIN_API:-/Applications/Unity/Hub/Editor/6000.5.9f1/Unity.app/Contents/Resources/PluginAPI}"
UNITY_PROJECT="${UNITY_PROJECT:-$HOME/Desktop/development/vehicle-hud-unity}"

# ABI=x86_64 builds for the x86_64 Cuttlefish AAOS image.
ABI="${ABI:-arm64-v8a}"
case "$ABI" in
    arm64-v8a) TRIPLE=aarch64-linux-android26 ;;
    x86_64) TRIPLE=x86_64-linux-android26 ;;
    *) echo "Unsupported ABI: $ABI" >&2; exit 1 ;;
esac

HERE="$(cd "$(dirname "$0")" && pwd)"
CXX="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/$TRIPLE-clang++"
GLSLC="$NDK/shader-tools/darwin-x86_64/glslc"
OUT_DIR="$HERE/build/$ABI"
GEN_DIR="$HERE/build/generated"
DEST_DIR="$UNITY_PROJECT/Assets/Plugins/Android/libs/$ABI"

mkdir -p "$OUT_DIR" "$GEN_DIR" "$DEST_DIR"

# Vulkan: SPIR-V as C array initializers.
"$GLSLC" -mfmt=c -o "$GEN_DIR/dial.vert.spv.inc" "$HERE/shaders/dial.vert"
"$GLSLC" -mfmt=c -o "$GEN_DIR/dial.frag.spv.inc" "$HERE/shaders/dial.frag"
"$GLSLC" -mfmt=c -o "$GEN_DIR/dial.comp.spv.inc" "$HERE/shaders/dial.comp"
# GLES: the same dial geometry, as a raw string compiled at runtime.
{ echo 'R"glsl('; cat "$HERE/shaders/dial_body.glsl"; echo ')glsl"'; } > "$GEN_DIR/dial_body.glsl.inc"

"$CXX" -shared -fPIC -O2 -std=c++17 -Wall -Wextra \
    -I "$UNITY_PLUGIN_API" -I "$GEN_DIR" \
    -static-libstdc++ \
    -o "$OUT_DIR/libhudgfx.so" "$HERE/hudgfx.cpp" \
    -lGLESv3 -llog
cp "$OUT_DIR/libhudgfx.so" "$DEST_DIR/libhudgfx.so"
echo "Built $OUT_DIR/libhudgfx.so -> $DEST_DIR"
