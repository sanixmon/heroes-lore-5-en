# Heroes Lore 5 — ARM32 on AArch64 (64-Bit Android) Technical Feasibility & Execution Report

## 1. Executive Summary & Status

- **Project Objective:** Advance from static ELF parsing and address space mapping to **actual ARM32 instruction execution**, and determine whether the compiled ARMv5TE native engine (`libHeroesLore5.so`) can realistically run inside a native 64-bit (`arm64-v8a`) Android application process.
- **Current Milestone Status:** **COMPLETE & VERIFIED BY EXECUTABLE TESTS**.
  - **Stage 1 (Controlled ARM32 Fixture):** 100% PASS across arithmetic, condition codes (CPSR), memory load/store, global data relocations, indirect function pointers, and PLT host trampoline callbacks (`poc/fixtures/test_arm32_fixture.so`).
  - **Stage 2 (Actual `libHeroesLore5.so` Engine Execution):** 100% PASS executing real game engine functions directly from the binary (`MIDASKernelManager::hash`, `StaticUtil::ByteToInt16`, `StaticUtil::ByteToInt32`, and `BFont::IsEscape`) with bit-level fidelity against ground truth.
  - **Full Automated Test Suite:** 9/9 tests passed (`tests/test_pipeline.py`).
- **Core Verdict:**
  1. **Instruction Execution:** Pure ARMv5TE execution is **technically proven and fully reproducible**. The absence of Thumb mode, NEON, and hardware VFP registers significantly reduces emulator complexity.
  2. **In-Process arm64 Android App Feasibility:** High engineering complexity due to Android 14/15 SELinux W^X memory policies, JNI 64-to-32-bit handle thunking, and potential vendor GLES 1.1 deprecation.
  3. **Production Distribution Path:** For modern 64-bit-only hardware (Pixel 7/8/9, Galaxy S24 Snapdragon 8 Gen 3), isolated 32-bit virtual containers (VMOS / F1 VM / Twoyi) remain the most immediate, zero-glitch deployment mechanism. For devices retaining 32-bit CPU cores, the updated manifest (`targetSdkVersion="28"`) installs natively.

```mermaid
flowchart TD
    subgraph AArch64 Host Process
        JVM["64-bit Android ART / JVM<br/>(sizeof(void*) = 8)"]
        Bridge["JNI Thunking Layer<br/>(Handle Table & 32-bit ID Pool)"]
        GLES_Bridge["GLES 1.1 Dispatch / GL4ES Wrapper<br/>(19 Fixed-Function APIs)"]
        Engine["ARM32 Execution Engine<br/>(Unicorn / CPU Core + ELF Loader)"]
    end

    subgraph 32-bit Virtual Address Space [< 4GB]
        SO["libHeroesLore5.so<br/>(ARMv5TE, 2 PT_LOAD segments, ~1.46 MB)"]
        VFS["Uncompressed VFS<br/>(assets/data.vfs.mp3)"]
        Stack["Simulated Stack<br/>(0x70000000, SP 8-byte aligned)"]
        Trampoline["Host PLT Trampoline Region<br/>(0x30000000, 64 KB)"]
    end

    JVM <--> Bridge
    Bridge <--> Engine
    Engine --> Trampoline
    Trampoline --> GLES_Bridge
    Engine <--> SO
    SO <--> Stack
    SO <--> VFS
```

---

## 2. Phase 1 — Audit of Prior PoC & Claim Classification

| Claim / Component | Evidence & Technical Review | Classification |
|---|---|:---:|
| **Target Architecture is ARMv5TE Soft-Float** | `readelf -A` confirms `Tag_CPU_arch: v5TE`, soft-float ABI (`__aeabi_*`). `readelf -s` confirms all 6,914 functions have bit 0 = 0 (pure ARM mode, 0 Thumb functions). | **Verified by static binary analysis** |
| **Only 77 Dynamic Imports across 6 Public NDK Libraries** | `readelf --dyn-syms` confirms 19 GLES 1.1, 54 libc/posix, 1 liblog, 3 libm. No private platform symbols. | **Verified by static binary analysis** |
| **Relocation Profile: 1,540 Relative, 75 Jump Slots** | `readelf -r` confirms 1,540 `R_ARM_RELATIVE`, 75 `R_ARM_JUMP_SLOT`, 2 `R_ARM_ABS32`, 1 `R_ARM_GLOB_DAT`. Handled cleanly in `poc/test_elf_loader.py`. | **Verified by executable tests** |
| **Virtual Memory Footprint is ~1.46 MB** | Exactly 2 `PT_LOAD` segments (`0x000000` R-E 1.45 MB, `0x162e98` RW 80 KB). | **Verified by executable tests** |
| **ARM32 Instruction Execution on Host** | Initial PoC only verified ELF mapping and prologues. Now implemented and validated in `poc/arm32_execution_engine.py`. | **Verified by executable tests** |
| **OpenGL ES 1.1 Rendering on Modern Android 15** | 19 fixed-function APIs identified. Driver availability varies across vendors; requires GLES1-to-GLES2 translation (GL4ES) on Vulkan-only devices. | **Partially verified** (Requires hardware testing) |
| **In-Process 64-Bit Memory Allocation (<4GB)** | `mmap` at `0x40000000` succeeds on host Linux and general AArch64. Subject to Android 14/15 SELinux W^X and `mmap_min_addr` policies. | **Partially verified** (Requires device testing) |
| **Cross-Bitness JNI Thunking Safety** | JNI functions and reverse callbacks mapped from strings and disassembly. Concrete handle table design documented in Phase 5. | **Partially verified** |

---

## 3. Phase 2 & 3 — ARM32 Instruction Execution & ELF Loading

### 3.1 Execution Strategy & Engine Selection
- **Selected Engine:** **Unicorn Engine** (QEMU CPU emulation core in ARM32 mode).
- **Justification:**
  - `libHeroesLore5.so` is compiled for **ARMv5TE** without Thumb-2, VFP hardware registers, or NEON extensions.
  - Unicorn provides cycle-accurate ARMv5TE instruction decoding, complete CPSR condition-code evaluation (EQ, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE), general-purpose registers (R0–R15), loads/stores with arbitrary alignments, and native memory hooking.
  - Avoids the maintenance overhead and SELinux fragility of abandoned projects like Tango while remaining far more complete than a custom instruction interpreter.

### 3.2 Controlled Test Fixture Validation (`poc/fixtures/test_arm32_fixture.c`)
Compiled with `clang -target armv5te-linux-androideabi -fuse-ld=lld -shared -nostdlib -O2 -fPIC`:

1. **Arithmetic & Bitwise:** `test_arithmetic(10, 20)`
   - Computes $(10+20)(10-20) + (((10 \oplus 20) \mid (10 \ \& \ 0\text{x0F})) \gg 1) = -300 + 15 = \mathbf{-285}$.
   - **Result:** **PASS** (R0 = `-285`).
2. **Conditionals, Loops & CPSR:** `test_conditionals_and_loops(10)`
   - Iterates $i=0..9$, branches on `i & 1`, updates accumulator across 10 loop cycles.
   - **Result:** **PASS** (R0 = `15`).
3. **Memory Load/Store & Endianness:** `test_memory_load_store(0x12345678)`
   - Writes 4 bytes to stack frame, reconstructs 32-bit unsigned integer via bit shifts.
   - **Result:** **PASS** (R0 = `0x12345678`).
4. **Global Data & Dynamic Relocations:** `test_global_data(100)`
   - Accesses `.data` global `g_magic_number` (`0xCAFEBABE`) via `R_ARM_GLOB_DAT` and `.rodata` string `g_greeting_str[0]` (`'H'` = 72).
   - Computes $0\text{xCAFEBABE} + 100 + 72 = \mathbf{0xCAFEBB6A}$.
   - **Result:** **PASS** (R0 = `0xCAFEBB6A`).
5. **Indirect Calls (Function Pointers):** `test_indirect_call(selector, x, y)`
   - Resolves static function pointers and branches via `bx r3`.
   - **Result:** **PASS** (`test_indirect_call(1, 40, 2) = 42`, `(0, 40, 2) = 38`).
6. **Host PLT Trampoline Callback:** `test_host_bridge(7)`
   - Branches to PLT stub for `host_bridge_multiply(7, 3)`; intercepted by Python host hook on trampoline page (`0x30000000`).
   - Host hook executes $7 \times 3 = \mathbf{21}$, sets R0, and returns cleanly to caller.
   - **Result:** **PASS** (R0 = `21`).

### 3.3 Direct Execution of Native `libHeroesLore5.so` Functions
`poc/arm32_execution_engine.py` loaded the actual `libHeroesLore5.so` binary at `0x40000000`, applied all 1,618 relocations, and executed live game engine functions:

1. **`MIDASKernelManager::hash(unsigned char*)` (`0x1516a4`):**
   - Live execution of the game's actual DJB2 hashing loop in native assembly:
     - Input `"c/csv/name.dat"` $\rightarrow$ **`0x53C493DA`** (matches ground truth VFS manifest).
     - Input `"version.txt"` $\rightarrow$ **`0x9ABED099`** (matches ground truth VFS manifest).
     - Input `"c/font/kor.fnt"` $\rightarrow$ **`0xF98A595F`** (matches ground truth VFS manifest).
     - Input `"c/csv/class.dat"` $\rightarrow$ **`0x7B82C28F`** (matches ground truth VFS manifest).
   - **Verdict:** **100% BIT-LEVEL MATCH**. Proves the native engine's instruction decoding, pointer offsets, and registers execute accurately.
2. **`StaticUtil::ByteToInt16(unsigned char*, int)` (`0x1437ec`):**
   - Positive test: Buffer offset 2 with `b"\x34\x12"` $\rightarrow$ returns **`4660`** (`0x1234`).
   - Negative signed test: Buffer offset 4 with `b"\xf0\xff"` $\rightarrow$ returns **`-16`** (`0xFFF0` sign-extended).
   - **Verdict:** **100% MATCH**.
3. **`StaticUtil::ByteToInt32(unsigned char*, int)` (`0x14379c`):**
   - Buffer offset 0 with `b"\x78\x56\x34\x12"` $\rightarrow$ returns **`0x12345678`**.
   - **Verdict:** **100% MATCH**.
4. **`BFont::IsEscape(char)` (`0x43668`):**
   - Tests character against patched escape table at `this + 0x18`:
     - `';'` $\rightarrow$ **`1`** (Line break escape).
     - `'~'` $\rightarrow$ **`1`** (Verified patched yellow escape tag).
     - `'\''` $\rightarrow$ **`0`** (Verified normal English apostrophe).
     - `'A'` $\rightarrow$ **`0`** (Normal printable character).
   - **Verdict:** **100% MATCH**.

---

## 4. Phase 4 — Runtime Compatibility Audit

| Subsystem | Audit Finding | Compatibility Requirement |
|---|---|---|
| **Bionic libc Compatibility** | The 54 imported libc symbols (`malloc`, `free`, `memcpy`, `open`, `read`, `write`, `close`, `socket`, `connect`, etc.) conform to standard C99/POSIX. | The host bridge can forward these directly to 64-bit libc stubs. `struct stat` and `time_t` require field-by-field translation if `stat` or `ftime` are invoked. |
| **Thread-Local Storage (TLS)** | `readelf -l` confirms zero `PT_TLS` segments. `libHeroesLore5.so` declares **no thread-local variables**. | Zero TLS setup required. |
| **Init & Fini Arrays** | Exactly one function pointer in `.init_array` (`0x162e98`, 4B) and one in `.fini_array` (`0x162e9c`, 4B). | Host loader must execute the 32-bit constructor before calling JNI methods. |
| **Stack Protection** | Uses `__stack_chk_guard` and calls `__stack_chk_fail` on canary mismatch. | Bound to 32-bit global variable mapped in low memory. |
| **libdl Behavior** | Does not import `dlopen` or `dlsym`. Self-contained native binary. | Dynamic symbol lookup at runtime is not performed by the engine. |

---

## 5. Phase 5 — JNI Boundary & Handle-Table Design

Crossing between a 64-bit ART environment (`sizeof(void*) == 8`) and the 32-bit engine (`sizeof(void*) == 4`):

```mermaid
sequenceDiagram
    participant ART as 64-bit Android ART (arm64-v8a)
    participant Bridge as JNI Thunk Bridge (AArch64)
    participant Engine as 32-bit HL5 Engine (ARM32)

    Note over Bridge: Setup 32-bit Handle Tables
    ART->>Bridge: Java calls nativeLoop()
    Bridge->>Engine: call_function(nativeLoop, env32, thiz32)
    Note over Engine: Executes TimerCb / Game Logic
    Engine->>Bridge: JNI Callback: getAssetSize(jstring32)
    Note over Bridge: Resolve string handle in 64-bit ART
    Bridge->>ART: Call Java CletEntry.getAssetSize(jstring64)
    ART-->>Bridge: Returns int size
    Bridge-->>Engine: Returns int size in R0
    Engine-->>Bridge: nativeLoop() completes
    Bridge-->>ART: Return to GLSurfaceView.Renderer
```

### Technical Design Rules:
1. **Opaque Handle Table for `jobject` / `jclass` / `jstring`:**
   - 64-bit `jobject` references must **never** be cast directly to 32-bit integers (causes pointer truncation).
   - The bridge maintains `std::vector<jobject> g_ref_table`. A 32-bit `jobject` is passed as an integer index `uint32_t handle`.
   - On native callback, `g_ref_table[handle]` recovers the 64-bit reference.
2. **`JNIEnv32` Struct Layout:**
   - The engine expects a 32-bit pointer to a table of 32-bit function pointers (`JNINativeInterface*`).
   - The bridge maps an array of 32-bit trampoline stubs in low memory (< 4GB) corresponding to JNIEnv functions used by `AndroidService`: `GetMethodID`, `CallStaticObjectMethod`, `CallStaticIntMethod`, `CallStaticVoidMethod`, `GetStringUTFChars`, `ReleaseStringUTFChars`.
3. **Memory Buffers & Direct Buffers:**
   - Arrays passed to `loadAsset(int, byte[], int)` must be mapped in the low 32-bit address space so the engine can access the buffer without address truncation.

---

## 6. Phase 6 — Graphics Compatibility (OpenGL ES 1.1)

### Audited OpenGL ES 1.1 API Calls (19 Symbols):
`glBindTexture`, `glColor4f`, `glCullFace`, `glDeleteTextures`, `glDisable`, `glDisableClientState`, `glDrawArrays`, `glEnable`, `glEnableClientState`, `glGenTextures`, `glLoadIdentity`, `glMatrixMode`, `glOrthof`, `glTexCoordPointer`, `glTexImage2D`, `glTexParameterf`, `glTexSubImage2D`, `glVertexPointer`, `glViewport`.

### Compatibility Analysis:
- **Renderer Role:** Used exclusively for 2D orthographic UI and sprite rendering.
  - Framebuffer: $400 \times 240$.
  - Projection: `glOrthof(0.0f, 400.0f, 240.0f, 0.0f, -1.0f, 1.0f)`.
  - Geometry: `glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)` for textured quads.
  - Blending: `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`.
- **Target Android Environment Support:**
  - If the device driver provides `/system/lib64/libGLESv1_CM.so`, the 64-bit host bridge can forward the 19 calls directly.
  - If the vendor driver is Vulkan/GLES3-only, **GL4ES** (or a custom 200-line GLES1-to-GLES2 shader wrapper) can intercept these 19 calls and render them onto an OpenGL ES 2.0/3.0 context via a standard orthographic MVP shader.

---

## 7. Phase 7 — Android 14/15 Security & Execution Constraints

1. **W^X Policy (Write XOR Execute):**
   - Android 14+ SELinux strictly enforces W^X on non-privileged application processes. Mapping memory with `PROT_WRITE | PROT_EXEC` triggers immediate kernel aborts.
   - **Compliance:** The loader maps segments with `PROT_READ | PROT_WRITE`, applies relocations, and uses `mprotect(PROT_READ | PROT_EXEC)` before instruction execution.
2. **Hardware Elimination of AArch32 Execution:**
   - ARM Cortex-X3, Cortex-X4, Cortex-A715, and Cortex-A720 physically lack AArch32 decoding hardware. Software dynamic translation (Unicorn/JIT) is **mandatory** on these devices.

---

## 8. Acceptance Criteria Verification

- [x] **1. Actual ARM32 test program executes under selected translation engine:** Verified via `poc/fixtures/test_arm32_fixture.so` executing under Unicorn Engine.
- [x] **2. Branching, memory access, function calls, and returns validated:** Verified in Stage 1 tests (arithmetic, CPSR conditionals/loops, memory load/store, indirect calls, host bridge).
- [x] **3. Test produces expected output:** All 6 fixture functions returned exact expected values.
- [x] **4. Actual `libHeroesLore5.so` engine instructions executed:** `MIDASKernelManager::hash` executed live in assembly and matched ground truth DJB2 hashes for 4 distinct assets; `StaticUtil` and `BFont` functions executed with bit-level fidelity.
- [x] **5. Implementation runs reproducibly:** Automated via `python3 poc/test_execution_engine.py` and `python3 tests/test_pipeline.py`.
- [x] **6. Host-side execution distinguished from Android execution:** Clear delineation documented in report.
- [x] **7. Localization assets and production build remain untouched:** English localization files, VFS assets, and `hl5-en.apk` remain intact and passing all regression checks.
