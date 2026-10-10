# Heroes Lore 5: Covenant of Darkness — English Localization

Reverse-engineering toolchain, translation catalog, and build pipeline for the complete English localization of **Heroes Lore 5** (*영웅서기5: 칠흑의 계약*, `co.kr.eamobile.HeroesLore5` v01.00.08).

---

## Overview

Heroes Lore 5 is a classic 2D mobile Action RPG developed by EA Mobile Korea. The Android release uses the EA Mobile **MIDAS** WIPI-to-Android port wrapper with a native ARM32 C++ game engine (`libHeroesLore5.so`) rendering a 400×240 framebuffer to OpenGL ES 1.x.

This repository contains the complete reverse-engineering specifications, binary parsers, compilers, translation catalog, automated test suite, and build pipeline for the English localization.

### Key Metrics
- **Resource Files Translated:** 315 files (57 CSV `.dat` tables + 258 `.scn` event/dialogue scripts).
- **Total Text Slots:** 15,241 slots (15,239 VFS string slots + 2 Android resource strings).
- **Unique Strings Translated:** 10,581 unique strings across Batches 1–4 (**0 untranslated**).
- **Target Language:** English.
- **Automated Tests:** 7/7 tests passed (**100% PASS** in `tests/test_pipeline.py`).
- **Binary Compatibility:** All field-specific C++ struct memory limits (19B, 20B, 27B, 199B, 254B, 255B) strictly obeyed.

---

## Architecture & Binary Specifications

### 1. Virtual File System (`assets/data.vfs.mp3`)
- **Structure:** Sequential stream of 2,189 entries with no central directory:
  ```c
  struct VfsEntry {
      uint32_t djb2_hash;     // DJB2 hash of normalized path (leading '/' stripped)
      uint32_t size;          // Payload size in bytes
      uint8_t  payload[size]; // Raw unencrypted asset bytes
  };
  ```
- **Storage:** Stored uncompressed (`STORED` / compression method 0) in the APK ZIP so Android `AssetManager.openFd` can pass a raw file descriptor to `nativeInitVFS`.
- **Manifest:** Entry [2187] (`hash = 0xbc909d54`) contains a 32-bit count (2,188) followed by all NUL-terminated filenames in archive order.

### 2. Database Tables (`c/csv/*.dat` & `c/csv2/*.dat` — 85 Files)
Outer container structure:
```c
struct CsvDatContainer {
    uint16_t row_count;
    struct Row {
        uint16_t row_size;           // Exact byte length of row payload
        uint8_t  row_payload[row_size];
    } rows[row_count];
};
```

#### Enforced Native C++ Buffer Safety Limits
| File Pattern | String Field | Verified C++ Memory Limit | Verified UI Pixel Width |
|---|---|---|---|
| `c/csv/buildup.dat` | `buildup_name` | **Fixed `char[20]` (max 19B)** | None |
| `c/csv/class.dat` | `class_name` | **Fixed `char[20]` (max 19B)** | None |
| `c/csv/enemy_*.dat`, `enemy_expert_*.dat` | `enemy_name` | **Fixed `char[20]` (max 19B)** | None |
| `c/csv/set_option.dat` | `set_name` | **Fixed `char[21]` (max 20B)** | None |
| `c/csv/item_{00..18}.dat` | `item_name` / `desc` | Name: **`char[21]` (max 20B)** / Desc: **`char[256]` (max 255B)** | 130px (`CommonUi`) |
| `c/csv/smith_{0..2}.dat` | `item_name` / `recipe` | Name: **`char[21]` (max 20B)** / Recipe: **`char[256]` (max 255B)** | 130px (`CommonUi`) |
| `c/csv/quest_{0..2}.dat` | `title` / `desc` / `cat` | Title & Cat: **`char[28]` (max 27B)** / Desc: **`char[200]` (max 199B)** | 140px (`setQuestViewInfo`) |
| `c/csv/{common,ingame,menu,mission}_text.dat` | `text` | Heap (`MC_knlCalloc`), **max 254B** | 156px (`StatePopup`) |
| `c/csv/tips.dat` | `tip` | Heap (`new char[L+1]`), **max 254B** | 340px (`StateGame::Paint`) |
| `c/csv2/help_*.dat` | `title` / `body` | Heap (`MC_knlCalloc`), **max 32,766B** | 130px (`CommonUi::DrawHelp`) |
| `c/ep/*/*.scn` & `c/map/*.scn` | `dialogue` / `map_title` | Dialogue: **max 512B** (auto-upgrades `strLenType` to `uint16_le`) / Title: **`char[52]` (max 51B)** | 384px (`Event_DialogWindow`) |

### 3. Font Engine (`BFont`) & Escape Patch
- **Character Metrics:** Single-byte ASCII glyphs advance by **6px/character** (`5px` glyph + `1px` spacing) via `c/font/eng.fnt`, height `11px`.
- **Apostrophe Patch (`lib/armeabi/libHeroesLore5.so` @ `0x157965`):**
  - In the original binary, `'` (`0x27`) was mapped to the yellow color escape code, causing English apostrophes (`Don't`, `Hero's`) to turn text yellow and disappear.
  - Byte `0x157965` is patched from `0x27` (`'`) to `0x7E` (`~`). Apostrophes render cleanly from `eng.fnt`, and `~` serves as the yellow color escape code.

---

## Tooling & Usage

Requirements: Python 3.8+, `apktool`, `zipalign`, `apksigner`.

### 1. Verify VFS Archive
```bash
python3 tools/hl5_vfs.py verify apktool_modern/assets/data.vfs.mp3
```

### 2. Validate Translations
```bash
python3 tools/validate_translations.py
```
Checks:
- Completeness across all batches (0 untranslated strings).
- Strict byte limits against C++ struct buffers.
- BFont word-wrapping at UI box boundaries (130px, 140px, 156px, 160px, 220px, 340px, 384px).
- Placeholder preservation (`#01..#13`, `#1..#2`, `%d`, `%s`, `EVENT_`).
- Absence of unescaped BFont color codes.
- Zero untranslated Hangul characters.

### 3. Run Automated Tests
```bash
python3 tests/test_pipeline.py
```

### 4. Build English APK
```bash
python3 tools/build_apk.py -o hl5-en.apk
```
The build pipeline automatically:
1. Recompiles all 57 CSV `.dat` files with updated row sizes and string lengths.
2. Recompiles all 258 `.scn` files with updated string tables.
3. Repacks `assets/data.vfs.mp3` deterministically.
4. Patches `libHeroesLore5.so` at offset `0x157965` (`'` $\rightarrow$ `~`).
5. Updates Android strings (`app_name` $\rightarrow$ `Heroes Lore 5`).
6. Rebuilds the APK with `apktool`.
7. Zipaligns to 4-byte boundaries.
8. Signs with `apksigner` (v1, v2, v3 schemes).

---

## Android Compatibility Notes

### Android 12 / 13 / 14 / 15
The rebuilt APK is configured for modern Android compatibility:
- `targetSdkVersion="28"` (bypasses Android 14/15's `INSTALL_FAILED_DEPRECATED_SDK_VERSION` block).
- `minSdkVersion="14"`.
- `android:exported="true"` explicitly declared on `HeroesLore5` launcher activity.
- Build-time Ant macro tags (`<if>`, `<switch>`) removed from manifest.
- Native libraries provided in both `lib/armeabi/` and `lib/armeabi-v7a/`.

### 64-Bit-Only Devices
Heroes Lore 5 uses a compiled 32-bit ARM engine (`libHeroesLore5.so`). Modern 64-bit-only phones (such as Google Pixel 7/8/9 and some Snapdragon 8 Gen 3 devices) have removed the 32-bit runtime:
- On 64-bit-only hardware, install the APK inside a 32-bit Android container such as **VMOS**, **F1 VM**, **Twoyi**, or **VPhoneGaGa**.
- On devices that still retain 32-bit hardware support, the APK installs directly. If installing via ADB on Android 14+:
  ```bash
  adb install --bypass-low-target-sdk-block hl5-en.apk
  ```

---

## Pre-Built Downloads

- **Latest English APK (Android 14/15 Compatible):** [Gofile Download](https://gofile.io/d/kRCMIMLs)
  - File: `hl5-en.apk`
  - Size: 18,478,102 bytes
  - SHA-256: `61ab40442b28bb69731ea2a9bde72c71137fa8b4d4324f8ca7e2ac30a1424d11`
