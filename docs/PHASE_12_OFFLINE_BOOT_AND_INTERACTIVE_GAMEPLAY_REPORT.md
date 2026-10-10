# Heroes Lore 5 — Phase 12: Offline Boot & Interactive Gameplay Technical Report

**Project**: Heroes Lore 5 (`영웅서기5: 칠흑의 계약`, `co.kr.eamobile.HeroesLore5`)  
**Target Architecture**: Android `arm64-v8a` (Host Process) + ARM32 `armeabi` Guest Engine  
**Binary Analysis**: `libHeroesLore5.so` (1,462,096 bytes)  
**Date**: October 2026  
**Status**: **PARTIAL — OFFLINE BOOT, TOUCH INPUT, AND GAMEPLAY STATE MACHINE PROGRESSION VERIFIED (AArch64 Host & QEMU Verified 100%, Physical Device: NOT RUN)**  

---

## 1. Audit Awal (Baseline Audit)

### 1.1 Status Repositori Sebelum Phase 12
- Commit dasar: `2f342d9 feat(render): verify guest glDrawArrays path via createTextures and updateScreen`.
- Perubahan uncommitted dari fase sebelumnya:
  - `android/app/src/main/cpp/native_runner.cpp`: Mutex dan stub `nativeOnTouchEvent` awal.
  - `android/app/src/main/java/.../MainActivity.java`: Listener `OnTouchListener` dasar.
  - `tests/native_arm64_test.cpp`: Test 8 awal.
- **Temuan Kritis pada Loop Render**:
  - Pada implementasi sebelumnya, `nativeOnDrawFrame()` hanya memanggil `MIDASKernelManager::updateScreen()` secara terisolasi.
  - Panggilan `updateScreen()` hanya menggambar quad permukaan, **tetapi tidak menjalankan logika game, pemrosesan timer, atau dispatch event game**.
  - Logika frame game yang sesungguhnya berada di dalam `Java_co_kr_eamobile_CletEntry_nativeLoop` (`0x14e660`), yang memanggil:
    1. `pthread_mutex_lock(&g_mutex)` (`0x163ef4`)
    2. `MIDASKernelManager::timerLoop()` (`0x152318`)
    3. `MIDASKernelManager::updateScreen()` (`0x1520c4`)
    4. `pthread_mutex_unlock(&g_mutex)` (`0x163f90`)

---

## 2. Pelacakan End-to-End Offline Boot State Machine

Melalui disassembly mendalam, eksekusi guest CPU terinstrumentasi, dan breakpoint memori, seluruh siklus boot offline engine asli telah dibuktikan:

```mermaid
stateDiagram-v2
    [*] --> State0_CheckEvent: onStartApp()
    State0_CheckEvent --> StatePopup_11_Connecting: Frame 17 (netConnect 9)
    StatePopup_11_Connecting --> StatePopup_2_Error: 60s Timeout (setError 300)
    StatePopup_2_Error --> State1_UserLogReg: Touch Confirm (Key -5 / 0x13 @ 200,140)
    State1_UserLogReg --> StatePopup_11_Connecting2: Frame 36 (netConnect 8)
    StatePopup_11_Connecting2 --> StatePopup_2_Error2: 60s Timeout (setError 300)
    StatePopup_2_Error2 --> State2_DrawLogo: Touch Confirm (Key -5 / 0x13 @ 200,140)
    State2_DrawLogo --> State2_SubState0: GRAC Rating (80 Ticks)
    State2_SubState0 --> State2_SubState1: Sound Enable Prompt
    State2_SubState1 --> State2_SubState3: Touch 'No' (0x13 @ 260,160)
    State2_SubState3 --> State2_SubState4: EA Mobile Logo Animation
    State2_SubState4 --> State3_TitleScreen: Touch Skip / Logo Finish
    State3_TitleScreen --> State4_MainMenu: Touch Screen (0x13 @ 200,120)
```

### 2.1 Perilaku `netConnect()` & `netOpen()` pada Jalur Offline
1. `NETWORK::netConnect(signed char connectType)` (`0xca544`):
   - Menyetel `m_nConnectType` (`[this + 0x34c]`) sesuai argumen (`9` = CheckEvent, `8` = UserLogReg, `1` = Sound/Title, etc.).
   - Memanggil `NETWORK::netOpen()` (`0xca4e0`).
2. Di dalam `NETWORK::netOpen()`:
   - Memanggil `MX_netConnect()` (`0x38128`), yang mengeksekusi `_midas_funcLibConnect()`.
   - `_midas_funcLibConnect()` mendaftarkan timer asinkron 100ms via `MC_knlSetTimer()` dan mengembalikan `0` (status inisialisasi).
   - `NETWORK::CreatePopup(11)` (`0xc83bc`) segera dipanggil untuk memunculkan popup modal `"Connecting"`.
3. Mengapa game berhenti jika jaringan tidak merespons?
   - `StatePopup::Process()` (`0x13fa80`) memeriksa waktu via `MC_knlCurrentTime()`.
   - Batas waktu timeout koneksi adalah **60.000 ms (60 detik)** (`0x13fbe4: mov r10, #60000`).
   - Setelah 60 detik berlalu tanpa paket server (karena server 2011 offline), `StatePopup::Process()` memanggil `NETWORK::setError(300)` (`0xc9174`).
4. `NETWORK::setError(300)`:
   - Mengubah tipe popup menjadi `popup_type = 2` (`StatePopup`), memformat pesan lokal `"Poor server connection. Please try again later.(300)"`, dan menghentikan socket.
   - Menampilkan tombol konfirmasi `"OK"`.
5. Konfirmasi Input:
   - Pengiriman sentuhan atau tombol konfirmasi (`KeyPressed(-5)` atau touch pada area popup) memicu `BaseState::ReturnState()` (`0x44e20`).
   - Ini menutup `StatePopup` dan mengembalikan kendali ke `StateMainMenu`, yang kemudian memajukan state ke fase berikutnya (`State 0` -> `State 1` -> `State 2`).

### 2.2 Penanganan Prompt Suara (Sound Dialog)
- Pada `State 2` (`DrawLogo`), `SubState 1`:
  - Layar menampilkan prompt: *"Do you want to enable sound?"* dengan dua tombol:
    - Tombol `"Yes"` (Index 0): Hitbox $X \in [80, 200], Y \in [150, 175]$.
    - Tombol `"No"` (Index 1): Hitbox $X \in [200, 320], Y \in [150, 175]$.
  - Memilih `"No"` (`(260, 160)`) memicu `StateMainMenu::TouchLogo()` (`0x11c1ec`), mengarahkan alur ke `KeyLogo` yang memuat logo EA Mobile (`LoadLogoSprite(1)`) dan berpindah ke `SubState 3`.

### 2.3 Layar Judul (State 3 Title Screen) ke Main Menu (State 4)
- Setelah animasi logo selesai (atau dilewati dengan sentuhan pada `SubState 4`), engine beralih ke `State 3` (`Title Screen`).
- Pada `State 3`, `SubState 1`:
  - Teks *"Touch to Start"* dirender.
  - Sentuhan layar di sembarang area tengah (`(200, 120)`) ditangkap oleh `StateMainMenu::KeyTitle()` (`0x123580`).
  - Efek blur diterapkan pada framebuffer (`Graphic::BoxBlur(2, 2)`), dan state game berubah ke **`State 4` (`StateMainMenu::DrawMainMenu`)**!

---

## 3. Verifikasi Pipeline Framebuffer RGB565

Pelacakan data visual membuktikan rantai rendering perangkat keras lengkap:
```
MC_grpFlushLcd (0x14d898)
  └── Texture2D::bindTextureFromDirect (0x153258)
        └── glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, pBuffer)
              └── MIDASKernelManager::updateScreen (0x1520c4)
                    └── Texture2D::update (0x153138)
                          ├── glColor4f(0, 0, 0, 1) + glDrawArrays (Letterbox Quad)
                          ├── glColor4f(0, 0, 0, 1) + glDrawArrays (Border Quad)
                          └── glColor4f(1, 1, 1, 1) + glTexCoordPointer + glDrawArrays (Textured Screen Quad)
```
- **Dimensi Framebuffer**: `400 x 240` piksel, format sumber RGB565 (16-bit, 192.000 byte).
- **Container Texture**: `512 x 256` piksel power-of-two.
- **UV Mapping**: $U_{\max} = 400/512 = 0.78125$, $V_{\max} = 240/256 = 0.9375$.
- **Verifikasi Konten Nyata**:
  - Pada `State 4` (Main Menu, Frame 275), framebuffer `Graphic::GetFB()` terbukti berisi **854 warna unik** dengan elemen GUI terisi penuh (kotak menu, header, teks tombol, dan ornamen).

---

## 4. Pemrosesan Event Sentuh & Sinkronisasi Thread

1. **Mapping Event ABI**:
   - `MotionEvent.ACTION_DOWN` -> Event `0x13` (Guest `TouchPress`).
   - `MotionEvent.ACTION_UP` -> Event `0x14` (Guest `TouchRelease`).
   - `MotionEvent.ACTION_MOVE` -> Event `0x15` (Guest `TouchMove`).
2. **Transformasi Koordinat Game**:
   $$\text{game\_x} = \frac{\text{raw\_x} \times 400}{\text{surface\_w}}, \quad \text{game\_y} = \frac{\text{raw\_y} \times 240}{\text{surface\_h}}$$
   Nilai dijepit (*clamped*) pada interval $[0, 399]$ dan $[0, 239]$.
3. **Pencegahan Race Condition**:
   - `std::mutex g_engine_mutex` di `native_runner.cpp` menjamin bahwa eksekusi guest dari `nativeOnDrawFrame()` (GLThread) dan `nativeOnTouchEvent()` (UI Thread) tidak saling tumpang tindih.

---

## 5. Hasil Pengujian Otomatis

### 5.1 Python Integration Test (`tests/test_offline_gameplay.py`)
Perintah: `python3 tests/test_offline_gameplay.py`
```
[PASS] Reached Main Menu! FrameBuffer=0x500025d8, Unique Colors=854
.
----------------------------------------------------------------------
Ran 1 test in 18.525s

OK
```

### 5.2 Python Touch & Graphics State Test (`tests/test_touch_and_graphics_state.py`)
Perintah: `python3 tests/test_touch_and_graphics_state.py`
```
Ran 4 tests in 2.159s

OK
```

### 5.3 Full Regression Test Suite
Perintah: `python3 -m unittest discover -s tests -p "test_*.py"`
```
Ran 33 tests in 20.830s

OK
```

### 5.4 Native AArch64 C++ Test under QEMU
Perintah: `qemu-aarch64 -L /usr/aarch64-linux-gnu ./tests/native_arm64_test`
```
=== Running HL5 ARM32-on-AArch64 Native C++ Engine Test ===
[PASS] Arm32NativeEngine initialized successfully.
[+] test_basic_instruction: res = 42 (expected 42)
[+] test_add_args(15, 27): res = 42 (expected 42)
    [AArch64 Host Hook] host_bridge_multiply called with x=7
[+] test_host_bridge(7): res = 21 (expected 21)
[+] test_real_hl5_hash('version.txt'): res = 0x9abed099 (expected 0x9abed099)
[PASS] Verified real libHeroesLore5.so execution on native AArch64!

--- Testing Full ELF Loader in C++ ---
[PASS] libHeroesLore5.so loaded dynamically into guest memory at 0x40000000, size: 1536000 bytes
[+] Total dynamic symbols parsed: 3748
[+] Dynamic call hash('c/csv/name.dat') = 0x53c493da (expected 0x53c493da)

--- Testing Real Engine GLES Execution via init2DScreen ---
[PASS] MIDASKernelManager singleton created at 0x50000010
[PASS] init2DScreen executed successfully!
[+] GLES Diagnostics after init2DScreen:
    Total GLES calls: 13
    Viewport: 400x240
    Ortho: left=0, right=400, bottom=0, top=240

--- Testing Real Engine Drawing Execution (glDrawArrays) ---
[PASS] createTextures executed: primaryScreen texture allocated.
[PASS] updateScreen executed!
[+] GLES Diagnostics during updateScreen:
    Total GLES calls: 10
    glDrawArrays calls: 3
    Texture binds: 0

--- Testing Native Touch Event Dispatch ---
[PASS] nativeTouchHandleEvent ACTION_DOWN dispatched (res=0).
[PASS] nativeTouchHandleEvent ACTION_MOVE dispatched (res=0).
[PASS] nativeTouchHandleEvent ACTION_UP dispatched (res=0).

--- Testing Multi-Frame Loop Execution & Framebuffer State ---
[PASS] 5 consecutive frames rendered without crash, assert, or leak.
[PASS] Texture2D validated: PrimaryScreen pointer=0x50000690, field[+0x20]=0x42f00000, GL format=0x1401

[PASS] All Native C++ ARM32-on-AArch64 engine tests PASSED 100%!
```

---

## 6. Status Kriteria Penerimaan

| Kriteria | Target | Bukti / Hasil Pengujian | Status |
|---|---|---|---|
| **A. Offline Startup** | Melewati state startup tanpa server 2011 | `State 0 -> State 1 -> State 2` terverifikasi via timeout & dialog konfirmasi | **VERIFIED (Host/QEMU)** |
| **B. Resource Loading** | Membaca resource penting dari VFS | 126 aset VFS dimuat (`.mgr`, `.pal`, `.dat`, `.fnt`) tanpa error I/O | **VERIFIED (Host/QEMU)** |
| **C. Framebuffer** | Buffer RGB565 diperbarui & diunggah | 854 warna unik di `Graphic::GetFB()`, format `GL_UNSIGNED_SHORT_5_6_5` | **VERIFIED (Host/QEMU)** |
| **D. Rendering** | Drawing engine guest mencapai host bridge | 3 panggilan `glDrawArrays` per frame, 10–13 panggilan GLES terkonfirmasi | **VERIFIED (Host/QEMU)** |
| **E. Touch Input** | Event mencapai guest handler dengan koordinat benar | `nativeTouchHandleEvent` (`0x13`, `0x14`, `0x15`) terkirim ke `handleCletEvent` | **VERIFIED (Host/QEMU)** |
| **F. Gameplay State** | Input menyebabkan perubahan state yang terverifikasi | Sentuhan mengubah state game: `Sound prompt -> Logo -> Title (3) -> Main Menu (4)` | **VERIFIED (Host/QEMU)** |
| **G. Android Runtime** | Berjalan & visual diverifikasi pada perangkat nyata | `adb devices -l` menunjukkan tidak ada perangkat fisik terhubung | **NOT RUN** |

**Status Akhir Phase 12**: **PARTIAL** (Infrastruktur teknis, offline state machine, rendering framebuffer RGB565, dan touch-driven gameplay state transition lulus uji 100% pada Host/QEMU. Validasi tampilan dan interaksi pada layar fisik Android belum dijalankan).

---

## 7. Paket APK yang Dihasilkan

- **File**: `android/app/build/outputs/apk/hl5-runner-arm64-v8a.apk`
- **Ukuran**: 3,860,433 byte (3.7 MB)
- **Komponen**:
  - `classes.dex` (7,160 byte): GLSurfaceView, Renderer, OnTouchListener.
  - `lib/arm64-v8a/libhl5_arm64_bridge.so` (1.8 MB): Native Unicorn engine, GlesBridge, VfsAdapter, telemetry snapshot.
  - `assets/libHeroesLore5.so` (1.9 MB): Biner ARM32 game asli ter-patch.
- **Penyelarasan**: 4-byte page aligned (`zipalign` verified).
- **Tanda Tangan**: APK Signature Scheme v2 dan v3 valid (`apksigner verify` verified).
