#!/usr/bin/env bash
set -euo pipefail

# Heroes Lore 5 - Clean Reproducible Android APK Build Script
# Uses Android NDK r25b Clang toolchain and Android SDK 34 build-tools

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NDK_BIN="/home/ubuntu/android-sdk/ndk/25.2.9519653/toolchains/llvm/prebuilt/linux-x86_64/bin"
SDK_PLATFORM="/home/ubuntu/android-sdk/platforms/android-34/android.jar"
BUILD_TOOLS="/home/ubuntu/android-sdk/build-tools/34.0.0"
KEYSTORE="${HOME}/.android/debug.keystore"

echo "=== 1. Clean Build Directory ==="
rm -rf "${ROOT_DIR}/android/app/build/intermediates"
mkdir -p "${ROOT_DIR}/android/app/build/intermediates/classes"
mkdir -p "${ROOT_DIR}/android/app/build/intermediates/dex"
mkdir -p "${ROOT_DIR}/android/app/build/intermediates/libs/arm64-v8a"
mkdir -p "${ROOT_DIR}/android/app/build/outputs/apk"

echo "=== 2. Compile Native ARM64 Shared Library (NDK r25b Clang++) ==="
"${NDK_BIN}/aarch64-linux-android26-clang++" -shared -fPIC -std=c++17 -O2 \
    -static-libstdc++ \
    -Wl,-z,max-page-size=16384 \
    -I"${ROOT_DIR}/android/app/src/main/cpp" \
    -I"${ROOT_DIR}/android/app/src/main/cpp/third_party/unicorn/include" \
    "${ROOT_DIR}/android/app/src/main/cpp/native_runner.cpp" \
    "${ROOT_DIR}/android/app/src/main/cpp/arm32_elf_loader.cpp" \
    "${ROOT_DIR}/android/app/src/main/cpp/arm32_native_engine.cpp" \
    "${ROOT_DIR}/android/app/src/main/cpp/gles_bridge.cpp" \
    "${ROOT_DIR}/android/app/src/main/cpp/vfs_adapter.cpp" \
    "${ROOT_DIR}/android/app/src/main/cpp/jni_bridge_core.cpp" \
    "${ROOT_DIR}/android/app/src/main/cpp/third_party/unicorn/lib/arm64-v8a/libunicorn.a" \
    -ldl -llog -lm \
    -o "${ROOT_DIR}/android/app/build/intermediates/libs/arm64-v8a/libhl5_arm64_bridge.so"

echo "=== 3. Compile Java Source Code (javac) ==="
javac -cp "${SDK_PLATFORM}" \
    -d "${ROOT_DIR}/android/app/build/intermediates/classes" \
    "${ROOT_DIR}/android/app/src/main/java/co/kr/eamobile/hl5/runner/MainActivity.java"

echo "=== 4. Convert Class Bytecode to Dalvik Executable (d8) ==="
"${BUILD_TOOLS}/d8" \
    --lib "${SDK_PLATFORM}" \
    --output "${ROOT_DIR}/android/app/build/intermediates/dex" \
    "${ROOT_DIR}/android/app/build/intermediates/classes/co/kr/eamobile/hl5/runner"/*.class

echo "=== 5. Package Base Resources (aapt) ==="
rm -rf /tmp/hl5_apk_pack && mkdir -p /tmp/hl5_apk_pack
"${BUILD_TOOLS}/aapt" package -f \
    -M "${ROOT_DIR}/android/app/src/main/AndroidManifest.xml" \
    -S "${ROOT_DIR}/android/app/src/main/res" \
    -I "${SDK_PLATFORM}" \
    -F /tmp/hl5_apk_pack/base.apk

echo "=== 6. Assemble Full APK (dex + native libraries + assets) ==="
python3 -c "
import zipfile
base_apk = '/tmp/hl5_apk_pack/base.apk'
unaligned_apk = '/tmp/hl5_apk_pack/unaligned.apk'
with zipfile.ZipFile(base_apk, 'r') as zin:
    with zipfile.ZipFile(unaligned_apk, 'w') as zout:
        for item in zin.infolist():
            zout.writestr(item, zin.read(item.filename))
        zout.write('${ROOT_DIR}/android/app/build/intermediates/dex/classes.dex', 'classes.dex', compress_type=zipfile.ZIP_DEFLATED)
        zout.write('${ROOT_DIR}/android/app/build/intermediates/libs/arm64-v8a/libhl5_arm64_bridge.so', 'lib/arm64-v8a/libhl5_arm64_bridge.so', compress_type=zipfile.ZIP_STORED)
        zout.write('${ROOT_DIR}/android/app/src/main/assets/libHeroesLore5.so', 'assets/libHeroesLore5.so', compress_type=zipfile.ZIP_STORED)
        zout.write('${ROOT_DIR}/android/app/src/main/assets/data.vfs.mp3', 'assets/data.vfs.mp3', compress_type=zipfile.ZIP_STORED)
"

echo "=== 7. Page Alignment (zipalign 4-byte) ==="
"${BUILD_TOOLS}/zipalign" -f -p 4 \
    /tmp/hl5_apk_pack/unaligned.apk \
    "${ROOT_DIR}/android/app/build/outputs/apk/hl5-runner-arm64-v8a.apk"

echo "=== 8. Sign APK (apksigner v2 & v3) ==="
"${BUILD_TOOLS}/apksigner" sign \
    --ks "${KEYSTORE}" \
    --ks-pass pass:android \
    --key-pass pass:android \
    "${ROOT_DIR}/android/app/build/outputs/apk/hl5-runner-arm64-v8a.apk"

echo "=== 9. Verification ==="
"${BUILD_TOOLS}/apksigner" verify --verbose "${ROOT_DIR}/android/app/build/outputs/apk/hl5-runner-arm64-v8a.apk"
sha256sum "${ROOT_DIR}/android/app/build/outputs/apk/hl5-runner-arm64-v8a.apk"
echo "Build complete and verified!"
