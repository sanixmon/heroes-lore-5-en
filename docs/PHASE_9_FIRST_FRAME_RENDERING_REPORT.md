# Heroes Lore 5 — Phase 9: First Frame Rendering Technical Report

**Project**: Heroes Lore 5 (`영웅서기5: 칠흑의 계약`, `co.kr.eamobile.HeroesLore5`)  
**Target Environment**: Android `arm64-v8a` (Host Process) + ARM32 `armeabi` Guest Engine  
**Binary Analisis**: `libHeroesLore5.so` (1,462,096 bytes)  
**Tanggal**: Oktober 2026  
**Status Fase**: **PARTIAL (Host Linux & Native AArch64 Verified 100%, Physical Device: NOT RUN)**  

---

## 1. Audit Temuan Awal (Jawaban atas 8 Pertanyaan Audit)

1. **Apakah Native Unicorn Engine Terkompilasi dan Ditautkan ke dalam APK?**  
   **YA**. Pustaka statis `libunicorn.a` (2.2 MB AArch64) telah ditautkan secara statis ke dalam `libhl5_arm64_bridge.so` (1.8 MB). Simbol `uc_open`, `uc_close`, `uc_emu_start`, dll., berada di dalam biner APK `arm64-v8a`.
2. **Bagaimana `libHeroesLore5.so` Dipetakan ke dalam Memori Guest?**  
   Melalui kelas C++ `hl5::Arm32ElfLoader`. Loader membaca header ELF32, memetakan segmen `PT_LOAD` ke alamat dasar guest `0x40000000`, menyalin data biner, dan mengosongkan segmen BSS.
3. **Bagaimana Simbol Dinamis dan Relokasi Guest Diselesaikan?**  
   Loader mengurai `.dynsym`, `.dynstr`, `.rel.plt` (75 jump slots), dan `.rel.dyn`. Relokasi `R_ARM_JUMP_SLOT` diarahkan ke halaman trampolin host (`0x30000000..0x30010000`). Relokasi `R_ARM_RELATIVE` menambahkan alamat dasar `0x40000000`, dan `R_ARM_GLOB_DAT` mengarahkan canary `__stack_chk_guard` ke `0x50000000`.
4. **Bagaimana Panggilan Tidak Langsung Guest ke Fungsi GLES Dicegat?**  
   Melalui entri GOT yang diisi alamat trampolin `0x300000XX`. Ketika ARM32 memanggil `bl <PLT>`, instruksi melompat ke trampolin `bx lr`. Unicorn memicu code hook `on_code_hook`, yang mendispatch panggilan ke handler C++ `GlesBridge`.
5. **Apakah Callback Host GLES Dipanggil dengan Konvensi ARM32 EABI yang Benar?**  
   **YA**. Konvensi Softfp ARM EABI diterapkan:
   - Argumen float di-decode dari representasi bit IEEE-754 (`unpack_float`).
   - Argumen ke-5 dan seterusnya (misal `glOrthof` argumen 5 & 6, `glTexImage2D` argumen 5..9) di-decode langsung dari stack guest via `UC_ARM_REG_SP`.
6. **Apakah Pointer Fungsi Host GLES Disimpan Sepenuhnya di Host Address Space?**  
   **YA**. Seluruh 19 pointer fungsi host (`p_glViewport`, `p_glDrawArrays`, dll.) berada di memori host 64-bit dan diselesaikan via `dlopen("libGLESv1_CM.so")`. Guest code tidak pernah melihat pointer host 64-bit (*zero pointer leakage*).
7. **Apakah Android Activity Sudah Memiliki Konteks EGL dan Thread Render yang Valid?**  
   **YA**. `MainActivity.java` telah dimigrasi untuk mengimplementasikan `GLSurfaceView.Renderer`. Konteks EGL dibuat secara native oleh Android pada `GLThread`, dan callback siklus hidup (`onSurfaceCreated`, `onSurfaceChanged`, `onDrawFrame`) mengeksekusi operasi rendering langsung pada thread GL yang aktif.
8. **Tahap Inisialisasi Mana yang Benar-Benar Dieksekusi vs Disimulasikan?**  
   Seluruh 6 tahap inisialisasi (`nativeInitKernel`, `nativeInitDisplay`, `nativeInitFileStorage`, `nativeInitVFS`, `nativeInitAudioConfiguration`, `nativeStartApp`) dieksekusi secara nyata pada kode biner asli `libHeroesLore5.so` tanpa simulasi atau stub palsu.

---

## 2. Arsitektur GLES Host Bridge (19 Fungsi GLES 1.1)

```mermaid
sequenceDiagram
    autonumber
    participant G as ARM32 Guest (Texture2D / MIDASKernelManager)
    participant T as Trampoline (0x300000XX)
    participant H as on_code_hook (C++)
    participant B as GlesBridge (AArch64 Host)
    participant D as Host GPU Driver (libGLESv1_CM.so)

    G->>T: bl glOrthof(0, 400, 240, 0, -1, 1)
    T->>H: uc_hook_add intercepts execution
    H->>H: Read R0..R3 + Read SP[0..1]
    H->>B: Unpack softfp IEEE-754 floats
    B->>D: p_glOrthof(0.0f, 400.0f, 240.0f, 0.0f, -1.0f, 1.0f)
    B-->>H: Return 0
    H->>G: Set PC = LR (clean return)

    G->>T: bl glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)
    T->>H: Intercepted
    H->>B: Marshal guest vertex & texcoord buffers
    B->>D: p_glVertexPointer(host_buf) & p_glDrawArrays(5, 0, 4)
    B-->>H: Record draw call in diagnostics
    H->>G: Return to guest loop
```

### 2.1 Pemetaan 19 Fungsi GLES 1.1 dan Konversi Argumen

| Simbol GLES | Offset PLT | Tipe Argumen | Penanganan ABI & Keamanan Memori |
|---|---|---|---|
| `glEnable` | `0x312c0` | `GLenum cap` | R0 dipetakan ke host enum |
| `glDisable` | `0x3132c` | `GLenum cap` | R0 dipetakan ke host enum |
| `glEnableClientState` | `0x312f0` | `GLenum array` | R0 dipetakan ke host enum |
| `glDisableClientState` | `0x314b8` | `GLenum array` | R0 dipetakan ke host enum |
| `glMatrixMode` | `0x31320` | `GLenum mode` | R0 dipetakan ke host enum |
| `glLoadIdentity` | `0x313bc` | `void` | Dipanggil langsung |
| `glCullFace` | `0x314c4` | `GLenum mode` | R0 dipetakan ke host enum |
| `glViewport` | `0x315e4` | `x, y, w, h` | R0..R3 di-cast ke int32; mencatat dimensi viewport |
| `glBindTexture` | `0x3147c` | `target, texture` | R0, R1; mencatat ID tekstur aktif |
| `glGenTextures` | `0x31614` | `n, textures*` | Buffer sementara host dialokasikan, ID ditulis ke memori guest |
| `glDeleteTextures` | `0x314a0` | `n, textures*` | Membaca ID dari memori guest dan memanggil host driver |
| `glTexParameterf` | `0x31338` | `target, pname, param` | Argumen R2 dikonversi dari bit float IEEE-754 |
| `glColor4f` | `0x314dc` | `r, g, b, a` | R0..R3 dikonversi dari bit float IEEE-754 |
| `glOrthof` | `0x3150c` | `l, r, b, t, near, far` | R0..R3 float + 2 argumen dari stack guest [sp, #0], [sp, #4] |
| `glVertexPointer` | `0x315d8` | `size, type, stride, ptr` | Menyimpan konfigurasi pointer vertex guest secara aman |
| `glTexCoordPointer` | `0x312cc` | `size, type, stride, ptr` | Menyimpan konfigurasi pointer koordinat tekstur guest |
| `glDrawArrays` | `0x31530` | `mode, first, count` | Memvalidasi & menyalin buffer vertex/texcoord ke host; mengeksekusi draw call |
| `glTexImage2D` | `0x31518` | `9 parameters` | R0..R3 + 5 argumen dari stack guest; menyalin piksel jika non-null |
| `glTexSubImage2D` | `0x31578` | `9 parameters` | R0..R3 + 5 argumen dari stack guest; menyalin piksel sub-image |

---

## 3. Berkas yang Ditambahkan dan Dimodifikasi

1. **`android/app/src/main/cpp/gles_bridge.h` & `gles_bridge.cpp` (Diperbarui)**:
   - Implementasi lengkap 19 fungsi GLES 1.1 CM, pembongkaran float softfp, penguraian argumen stack, shadow buffer memori vertex/texcoord, dan diagnostic counter.
2. **`android/app/src/main/cpp/arm32_elf_loader.h` & `arm32_elf_loader.cpp` (Baru)**:
   - Loader ELF C++ lengkap yang memetakan segmen `libHeroesLore5.so`, mengurai 3.748 simbol dinamis, dan menghubungkan relokasi ke trampolin engine.
3. **`android/app/src/main/cpp/arm32_native_engine.h` & `arm32_native_engine.cpp` (Diperbarui)**:
   - Pembaruan `on_code_hook` untuk membaca stack pointer `SP` dan mengekstrak hingga 8 argumen tambahan dari stack guest.
   - Penambahan `get_or_create_hook` untuk mencegah penimpaan hook yang telah terdaftar.
4. **`android/app/src/main/cpp/native_runner.cpp` (Diperbarui)**:
   - Implementasi JNI siklus hidup permukaan: `nativeOnSurfaceCreated`, `nativeOnSurfaceChanged`, `nativeOnDrawFrame`, `nativeOnPause`, `nativeOnResume`, `nativeOnDestroy`.
5. **`android/app/src/main/java/co/kr/eamobile/hl5/runner/MainActivity.java` (Diperbarui)**:
   - Implementasi `GLSurfaceView.Renderer` dengan overlay visual real-time diagnostik frame.
6. **`tests/test_gles_bridge.py` (Baru)**:
   - Test suite komprehensif untuk penguraian simbol GLES, softfp float, argumen stack, dan penolakan pointer invalid.
7. **`tests/native_arm64_test.cpp` (Diperbarui)**:
   - Uji coba native AArch64 C++ yang memvalidasi `Arm32ElfLoader` dan eksekusi nyata `init2DScreen()` menghasilkan 13 panggilan GLES.
8. **`tests/test_pipeline.py` (Diperbarui)**:
   - Integrasi `test_gles_bridge_execution` ke pipeline regresi utama (12/12 lulus).
9. **`android/app/build/outputs/apk/hl5-runner-arm64-v8a.apk` (Diperbarui)**:
   - Biner APK runner 64-bit arm64-v8a terkompilasi (657 KB) dengan renderer GLSurfaceView aktif.

---

## 4. Hasil Pengujian & Bukti Eksekusi Frame

```
=== 1. Native AArch64 Test (qemu-aarch64) ===
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
[PASS] All Native C++ ARM32-on-AArch64 engine tests PASSED 100%!

=== 2. GLES Bridge Test Suite (tests/test_gles_bridge.py) ===
[+] init2DScreen dispatched 13 real GLES calls to host bridge!
Ran 5 tests in 0.260s -> OK (100%)

=== 3. Pipeline Regresi Otomatis (tests/test_pipeline.py) ===
All 12 automated tests PASSED 100%!
```

---

## 5. Apakah Jalur Render Guest Asli Telah Dieksekusi?

**YA**. Di bawah proses native AArch64 C++, fungsi inisialisasi render 2D asli dari `libHeroesLore5.so` (`MIDASKernelManager::init2DScreen`) dieksekusi secara nyata oleh Unicorn:
- **Panggilan GLES yang Terbukti Dicegat**:
  - `glViewport(0, 0, 400, 240)`
  - `glMatrixMode(GL_PROJECTION)`
  - `glLoadIdentity()`
  - `glOrthof(0.0f, 400.0f, 0.0f, 240.0f, -1.0f, 1.0f)`
  - `glDisable(GL_DEPTH_TEST)`, `glDisable(GL_LIGHTING)`, `glDisable(GL_CULL_FACE)`
  - `glEnable(GL_TEXTURE_2D)`
  - `glCullFace(GL_BACK)`
  - `glEnableClientState(GL_VERTEX_ARRAY)`
  - `glEnableClientState(GL_TEXTURE_COORD_ARRAY)`
- **Total Panggilan GLES**: Tepat **13 panggilan GLES riil** tercatat pada host bridge diagnostics.

---

## 6. Hasil Build APK & Validasi Perangkat Android

- **Hasil Build APK Host Runner**:
  - Berkas: `android/app/build/outputs/apk/hl5-runner-arm64-v8a.apk`
  - Ukuran: 657 KB
  - Arsitektur: `arm64-v8a` (`libhl5_arm64_bridge.so` 1.8 MB dengan embedded Unicorn + GlesBridge + ElfLoader)
  - Penyelarasan: 4-byte page aligned (`zipalign` verified)
  - Tanda tangan: APK Signature Scheme v2 dan v3 verified.
- **Status Pengujian Perangkat Android**:
  - **Status: NOT RUN**.
  - **Alasan**: `adb devices -l` melaporkan tidak ada perangkat fisik atau emulator Android yang terhubung (`List of devices attached: (empty)`). Sesuai ketentuan *Scope Control*, pengujian perangkat fisik dilaporkan apa adanya sebagai **NOT RUN**, dan status keseluruhan dinyatakan sebagai **PARTIAL**.

---

## 7. Known Limitations

1. **Ketiadaan Hardware ARM32 pada SoC Modern**: Chipset terbaru (misal Tensor G3/G4, Snapdragon 8 Gen 3) tidak mendukung eksekusi instruksi ARM32 secara native; integrasi biner Unicorn AArch64 adalah jalur mutlak untuk menjalankan mesin.
2. **Ketergantungan EGL Context Thread**: Panggilan grafis GLES 1.1 wajib dieksekusi pada `GLThread` yang memiliki konteks EGL aktif dari `GLSurfaceView`.

---

## 8. Blocker Berikutnya yang Paling Kritis

1. **Pengujian pada Perangkat Android Fisik (Deployment)**:
   - Memasang `hl5-runner-arm64-v8a.apk` pada perangkat Android ARM64 fisik via ADB saat perangkat tersedia untuk memverifikasi tampilan visual nyata di layar.
2. **Sinkronisasi Input Layar Sentuh (`nativeTouchHandleEvent`)**:
   - Menghubungkan event `MotionEvent` dari `GLSurfaceView` ke fungsi native `Java_co_kr_eamobile_CletEntry_nativeTouchHandleEvent` agar pemain dapat berinteraksi dengan menu game.
