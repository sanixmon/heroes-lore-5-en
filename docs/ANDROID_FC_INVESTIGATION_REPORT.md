# Heroes Lore 5 — Android Crash Investigation & Native Runtime Hardening Report

## 1. Executive Summary

- **Issue Reported**: The APK `hl5-runner-arm64-v8a.apk` force-closed when installed and launched on a physical Android arm64-v8a device. Host and QEMU tests were passing, creating a discrepancy between emulator testing and actual Android OS execution.
- **Root Cause Identified**: The native library `libhl5_arm64_bridge.so` was previously linked against **glibc** dynamic libraries (`libc.so.6`, `libm.so.6`, `libstdc++.so.6`, `ld-linux-aarch64.so.1`) instead of Android Bionic, because the underlying `libunicorn.a` static archive had been compiled on host Ubuntu with GNU toolchains rather than the Android NDK. When the Android dynamic linker (`/system/bin/linker64`) attempted `dlopen("libhl5_arm64_bridge.so")` via `System.loadLibrary`, it failed immediately with an unresolved symbol / missing glibc runtime error, causing an `UnsatisfiedLinkError` or SIGABRT on Android before reaching the first frame.
- **Secondary Latent Risks Addressed**:
  1. **Page Alignment**: Android 14/15/16 16KB memory page compatibility was enforced using `-Wl,-z,max-page-size=16384`.
  2. **GLES 1.1 Availability**: Modern Android devices lack `/system/lib64/libGLESv1_CM.so`. Added safe fallback checks and non-crashing hooks in `GlesBridge`.
  3. **Telemetry & Exception Barriers**: Added native Android `<android/log.h>` logging tags (`HL5_Runner`, `HL5_NativeRunner`, `HL5_GlesBridge`, `HL5_Arm32Engine`) and a Java `Thread.setDefaultUncaughtExceptionHandler` so crash traces surface directly in `logcat`.
- **Status of Android Runtime**: **NOT RUN (Physical Device / Emulator)**. The workstation cloud environment has no attached ADB devices (`adb devices -l` returns empty). The failure is not reproducible in this headless container. Android execution stability is therefore reported as **NOT RUN**, pending logcat verification from the user's test device.

---

## 2. Deep-Dive Crash Stage & Root Cause Analysis

### 2.1 Dynamic Linker Failure (Glibc vs Android Bionic)
- **Previous `libhl5_arm64_bridge.so` Dynamic Section**:
  ```text
  0x0000000000000001 (NEEDED) Shared library: [libstdc++.so.6]
  0x0000000000000001 (NEEDED) Shared library: [libm.so.6]
  0x0000000000000001 (NEEDED) Shared library: [libgcc_s.so.1]
  0x0000000000000001 (NEEDED) Shared library: [libc.so.6]
  0x0000000000000001 (NEEDED) Shared library: [ld-linux-aarch64.so.1]
  ```
- **Why this caused instant Force-Close on Android**:
  Android devices run Google Bionic libc (`libc.so`), not GNU C library (`libc.so.6`). They have no `ld-linux-aarch64.so.1` interpreter and no `libstdc++.so.6`.
  When Android loads an APK via `System.loadLibrary("hl5_arm64_bridge")`, `bionic/linker` parses the `DT_NEEDED` list. Encountering `libc.so.6` immediately aborts process loading with `java.lang.UnsatisfiedLinkError: dlopen failed: library "libc.so.6" not found`.
- **Fix Applied**:
  - Recompiled the entire Unicorn 2.1.2 engine from source using **Android NDK r25b (25.2.9519653)** Clang 14 targeting `aarch64-linux-android26`.
  - Re-linked `libhl5_arm64_bridge.so` using NDK Clang++ with `-static-libstdc++`, `-ldl`, `-llog`, and `-lm`.
  - **New `libhl5_arm64_bridge.so` Dynamic Section**:
    ```text
    0x0000000000000001 (NEEDED) Shared library: [libdl.so]
    0x0000000000000001 (NEEDED) Shared library: [liblog.so]
    0x0000000000000001 (NEEDED) Shared library: [libm.so]
    0x0000000000000001 (NEEDED) Shared library: [libc.so]
    ```
    All dependencies are now 100% native Android Bionic libraries present on all Android 8.0+ (API 26+) devices.

---

### 2.2 16KB Memory Page Alignment
- Many modern Android devices (Android 14, Android 15, and Android 16 previews on Pixel 8/9) enforce 16KB page sizes.
- Shared libraries built with default 4KB ELF alignments will crash at load time with `ELF alignment 4096 is smaller than page size 16384`.
- **Fix Applied**: Linked with `-Wl,-z,max-page-size=16384`. All `PT_LOAD` segments in `libhl5_arm64_bridge.so` now have `Align: 0x4000` (16,384 bytes).

---

### 2.3 GLES 1.1 Dynamic Loading on Android
- Modern 64-bit Android devices typically omit `libGLESv1_CM.so` in `/system/lib64/`.
- In `gles_bridge.cpp`, if `dlopen("libGLESv1_CM.so")` fails, the bridge now gracefully flags `gles1_supported_ = false` and routes calls to safe internal stubs with diagnostic warnings rather than dereferencing null function pointers.

---

### 2.4 Diagnostic Logging & Uncaught Exception Handler
- Added Android Logcat integration across the native runner:
  - `LOG_TAG: HL5_Runner` (Java activity and lifecycle)
  - `LOG_TAG: HL5_NativeRunner` (JNI surface creation, ELF loading, singleton creation, frame render)
  - `LOG_TAG: HL5_GlesBridge` (GLES provider resolution and draw diagnostics)
  - `LOG_TAG: HL5_Arm32Engine` (Guest instruction logging via guest `__android_log_print`)
- Configured a global `Thread.UncaughtExceptionHandler` in `MainActivity.java` so any runtime Java exception is written to Android Logcat before termination.

---

## 3. Verification & Evidence

### 3.1 Verification Matrix
| Component / Check | Method | Expected | Actual Result | Status |
|---|---|---|---|---|
| **NDK Bionic DT_NEEDED** | `readelf -d libhl5_arm64_bridge.so` | Only `libdl.so`, `liblog.so`, `libm.so`, `libc.so` | Matched exactly; zero glibc symbols | **PASS** |
| **16KB Page Alignment** | `readelf -l libhl5_arm64_bridge.so` | `Align = 0x4000` on all LOAD segments | `Align = 0x4000` verified | **PASS** |
| **QEMU AArch64 Native** | `qemu-aarch64 /tmp/native_arm64_test` | All 8 phases pass with NDK `libunicorn.a` | 100% passed (GLES calls: 13, DrawArrays: 3, Touches: 3) | **PASS** |
| **Full Python Test Suite** | `python3 -m unittest discover tests` | 34 unit and integration tests | 34/34 tests passed in 53.6s | **PASS** |
| **APK Alignment (zipalign)** | `zipalign -c -v 4 apk` | 4-byte aligned | Verification successful | **PASS** |
| **APK Signature (apksigner)** | `apksigner verify --verbose` | v2 & v3 valid | Scheme v2: true, Scheme v3: true | **PASS** |
| **Real Device / Emulator** | `adb devices -l` | Device attached | No physical device attached | **NOT RUN** |

---

## 4. Instructions for Real-Device Logcat Capture

To obtain conclusive runtime evidence from an Android device:
```bash
# Clear logcat buffers
adb logcat -c

# Launch the app
adb shell am start -n co.kr.eamobile.hl5.runner/.MainActivity

# Filter logs for HL5 tags and native crashes
adb logcat -v time -s HL5_Runner:V HL5_NativeRunner:V HL5_GlesBridge:V HL5_Arm32Engine:V DEBUG:* AndroidRuntime:*
```
If any crash persists, provide the output of the command above to determine the exact failure stage.
