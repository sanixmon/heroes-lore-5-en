#include <jni.h>
#include <string>
#include <sstream>
#include "jni_bridge_core.h"
#include "gles_bridge.h"
#include "vfs_adapter.h"
#include "arm32_native_engine.h"
#include "arm32_elf_loader.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <cerrno>
#include <mutex>
#include <atomic>

namespace {
hl5::Arm32NativeEngine g_native_engine;
hl5::Arm32ElfLoader    g_elf_loader;
hl5::GlesBridge        g_gles_bridge;
hl5::VfsAdapter        g_vfs_adapter;
std::mutex             g_engine_mutex;
uint32_t               g_km_instance = 0;
uint32_t               g_game_instance = 0;
int32_t                g_surface_w = 400;
int32_t                g_surface_h = 240;
uint64_t               g_frame_counter = 0;
std::atomic<uint32_t>  g_touch_events_received{0};
std::atomic<uint32_t>  g_touch_events_dispatched{0};
int32_t                g_last_touch_type = -1;
int32_t                g_last_touch_raw_x = -1;
int32_t                g_last_touch_raw_y = -1;
int32_t                g_last_touch_game_x = -1;
int32_t                g_last_touch_game_y = -1;
int32_t                g_current_game_state = -1;
int32_t                g_current_game_substate = -1;
int32_t                g_current_game_popup = -1;
bool                   g_engine_ready = false;
std::string            g_last_frame_status = "Awaiting first frame";
}
extern "C" {

JNIEXPORT jstring JNICALL
Java_co_kr_eamobile_hl5_runner_MainActivity_nativeRunDiagnosticTests(
    JNIEnv* env,
    jobject thiz,
    jstring internalPathStr,
    jstring externalPathStr) {
    std::ostringstream oss;
    oss << "=== Heroes Lore 5 — ARM64 Host Bridge Diagnostics ===\n";

    // 1. Handle Table Verification
    hl5::TypedHandleTable handle_table;
    int test_payload = 12345;
    uint32_t hid = handle_table.allocate(hl5::HandleType::LOCAL_REF, &test_payload, "TestClass", "()V");
    oss << "[+] Handle Allocation: Handle ID 0x" << std::hex << hid << std::dec << "\n";

    void* resolved = handle_table.resolve(hid, hl5::HandleType::LOCAL_REF);
    if (resolved == &test_payload) {
        oss << "[PASS] Handle resolved correctly without 64-bit pointer truncation.\n";
    } else {
        oss << "[FAIL] Handle resolution mismatch!\n";
    }

    // Local frame scope pop
    handle_table.push_local_frame();
    uint32_t hid_local = handle_table.allocate(hl5::HandleType::LOCAL_REF, &test_payload);
    handle_table.pop_local_frame();
    try {
        handle_table.resolve(hid_local, hl5::HandleType::LOCAL_REF);
        oss << "[FAIL] Failed to detect use-after-free on popped local frame!\n";
    } catch (const std::exception& e) {
        oss << "[PASS] Scoped local frame lifecycle verified: " << e.what() << "\n";
    }

    // 2. Address Space & W^X Verification
    hl5::GuestAddressSpace addr_space;
    if (addr_space.initialize(16 * 1024 * 1024)) {
        oss << "[PASS] 32-bit Guest Address Space initialized (Base: 0x"
            << std::hex << addr_space.base_vaddr() << ", Size: 16MB)\n" << std::dec;

        uint32_t code_ptr = addr_space.allocate_guest(4096);
        if (code_ptr != 0 && addr_space.make_executable(code_ptr, 4096)) {
            oss << "[PASS] Strict SELinux W^X transition verified on guest page 0x"
                << std::hex << code_ptr << std::dec << "\n";
        } else {
            oss << "[WARN] SELinux W^X transition failed or restricted.\n";
        }
    } else {
        oss << "[FAIL] Failed to initialize 32-bit guest address space.\n";
    }

    // 3. OpenGL ES 1.1 Interface Audit
    hl5::GlesBridge gles_bridge;
    bool gles_ok = gles_bridge.initialize();
    oss << "[+] GLES 1.1 Audit: Provider = " << gles_bridge.gles_provider_name()
        << ", GLES 1.1 Supported = " << (gles_ok ? "YES" : "NO (GL4ES Needed)") << "\n";

    // 4. Virtual File System (VFS) Adapter Diagnostics
    hl5::VfsAdapter vfs_adapter;
    // Create a temporary pipe / file to simulate game asset
    char tmp_path[] = "/data/local/tmp/vfs_test_XXXXXX";
    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        // Fallback for restricted app sandbox without /data/local/tmp
        char local_tmp[] = "/tmp/vfs_test_XXXXXX";
        tmp_fd = mkstemp(local_tmp);
    }
    if (tmp_fd >= 0) {
        const char test_data[] = "VFS_TEST_HEADER_PAYLOAD_1234567890";
        write(tmp_fd, test_data, sizeof(test_data));
        lseek(tmp_fd, 0, SEEK_SET);

        uint32_t guest_fd = vfs_adapter.register_asset(tmp_fd, 4, 15); // Offset 4, Length 15
        oss << "[+] VFS Registered: Guest FD 0x" << std::hex << guest_fd << std::dec << "\n";

        uint32_t dup_fd = vfs_adapter.hook_dup(guest_fd);
        uint32_t fp_handle = vfs_adapter.hook_fdopen(dup_fd, "rb");
        if (fp_handle > 0) {
            oss << "[PASS] VFS Stream handle allocated: 0x" << std::hex << fp_handle << std::dec << " (No 64-bit truncation)\n";
            vfs_adapter.hook_fseek(fp_handle, 0, SEEK_SET);

            uint32_t guest_buf = addr_space.allocate_guest(64);
            size_t read_items = vfs_adapter.hook_fread(guest_buf, 1, 10, fp_handle, addr_space);
            if (read_items == 10) {
                char* read_ptr = static_cast<char*>(addr_space.to_host_ptr(guest_buf));
                std::string read_str(read_ptr, 10);
                oss << "[PASS] VFS Read verified across offset: \"" << read_str << "\"\n";
            } else {
                oss << "[FAIL] VFS Read item mismatch!\n";
            }

            // Out-of-bounds guest memory validation check
            size_t oob_read = vfs_adapter.hook_fread(0xFFFFFF00, 1, 10, fp_handle, addr_space);
            if (oob_read == 0) {
                oss << "[PASS] Out-of-bounds guest memory read rejected safely.\n";
            } else {
                oss << "[FAIL] Out-of-bounds read was not rejected!\n";
            }

            vfs_adapter.hook_fclose(fp_handle);
        } else {
            oss << "[FAIL] VFS fdopen failed!\n";
        }
        close(tmp_fd);
    } else {
        oss << "[WARN] Unable to create temp file for VFS descriptor test in current sandbox.\n";
    }

    // 5. Native ARM32 Unicorn CPU Execution on AArch64
    hl5::Arm32NativeEngine native_engine;
    if (native_engine.initialize()) {
        oss << "[PASS] Native ARM32 Unicorn CPU Engine initialized on AArch64.\n";

        uint32_t code_addr = 0x40000000;
        native_engine.map_memory(code_addr, 4096, UC_PROT_ALL);
        uint32_t test_code[] = { 0xe3a0002a, 0xe12fff1e }; // mov r0, #42; bx lr
        native_engine.write_memory(code_addr, test_code, sizeof(test_code));
        uint32_t res = native_engine.call_function(code_addr, {});
        if (res == 42) {
            oss << "[PASS] Native ARM32 Instruction Execution verified (Result: " << res << " == 42).\n";
        } else {
            oss << "[FAIL] Native ARM32 Instruction Execution mismatch: " << res << "\n";
        }

        // Test Host Bridge Callback
        uint32_t bridge_tramp = native_engine.register_host_hook("host_test_hook", [](const std::vector<uint32_t>& args) -> uint32_t {
            return args[0] * 3;
        });
        uint32_t host_code_addr = 0x40001000;
        native_engine.map_memory(host_code_addr, 4096, UC_PROT_ALL);
        uint32_t host_test_code[] = {
            0xe59f3004, // ldr r3, [pc, #4]
            0xe12fff33, // blx r3
            0xe12fff1e, // bx lr
            bridge_tramp
        };
        native_engine.write_memory(host_code_addr, host_test_code, sizeof(host_test_code));
        uint32_t host_res = native_engine.call_function(host_code_addr, {7});
        if (host_res == 21) {
            oss << "[PASS] Native Host Bridge Callback dispatch verified (Result: " << host_res << " == 21).\n";
        } else {
            oss << "[FAIL] Native Host Bridge Callback mismatch: " << host_res << "\n";
        }
    } else {
        oss << "[FAIL] Failed to initialize native ARM32 Unicorn CPU Engine.\n";
    }

    // 6. File Storage Initialization & Sandbox Verification
    const char* internal_chars = env->GetStringUTFChars(internalPathStr, nullptr);
    const char* external_chars = env->GetStringUTFChars(externalPathStr, nullptr);
    std::string int_dir = internal_chars ? internal_chars : "";
    std::string ext_dir = external_chars ? external_chars : "";
    if (internal_chars) env->ReleaseStringUTFChars(internalPathStr, internal_chars);
    if (external_chars) env->ReleaseStringUTFChars(externalPathStr, external_chars);

    oss << "[+] Storage Sandbox Configuration:\n";
    oss << "    Internal: " << int_dir << "\n";
    oss << "    External: " << ext_dir << "\n";

    if (!int_dir.empty()) {
        // Test sandbox directory creation
        std::string test_subdir = int_dir + "/hl5_save";
        mkdir(test_subdir.c_str(), 0700);

        // Test sandbox file creation and read/write
        std::string test_save_file = test_subdir + "/savedata.dat";
        int sfd = open(test_save_file.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (sfd >= 0) {
            const char save_hdr[] = "HL5_SAVE_RECORD_V1";
            write(sfd, save_hdr, sizeof(save_hdr));
            close(sfd);

            // Verify read back
            sfd = open(test_save_file.c_str(), O_RDONLY);
            if (sfd >= 0) {
                char read_hdr[32] = {0};
                read(sfd, read_hdr, sizeof(save_hdr));
                close(sfd);
                if (strcmp(read_hdr, save_hdr) == 0) {
                    oss << "[PASS] File Storage Sandbox: directory & save file verified in app-private storage.\n";
                } else {
                    oss << "[FAIL] Save file readback mismatch.\n";
                }
                unlink(test_save_file.c_str());
            }
            rmdir(test_subdir.c_str());
        } else {
            oss << "[WARN] Could not create test file in private directory: " << strerror(errno) << "\n";
        }
    }

    return env->NewStringUTF(oss.str().c_str());
}

JNIEXPORT void JNICALL
Java_co_kr_eamobile_hl5_runner_MainActivity_nativeOnSurfaceCreated(
    JNIEnv* env,
    jobject /*thiz*/,
    jstring internalPathStr,
    jstring externalPathStr) {
    // Runs on GLThread with active EGLContext!
    g_frame_counter = 0;
    g_engine_ready = false;

    // 1. Initialize GLES Bridge with host driver
    g_gles_bridge.initialize();
    g_gles_bridge.reset_diagnostics();

    // 2. Initialize Native ARM32 Unicorn Engine
    if (!g_native_engine.initialize()) {
        g_last_frame_status = "[FAIL] Engine initialization failed on GLThread";
        return;
    }

    // 3. Register GLES hooks
    g_gles_bridge.register_with_engine(g_native_engine);

    // 4. Try loading libHeroesLore5.so from candidate locations
    const char* int_chars = internalPathStr ? env->GetStringUTFChars(internalPathStr, nullptr) : nullptr;
    std::string internal_so = (int_chars && int_chars[0] != '\0') ? (std::string(int_chars) + "/libHeroesLore5.so") : "";
    if (int_chars) env->ReleaseStringUTFChars(internalPathStr, int_chars);

    const char* so_candidates[] = {
        internal_so.empty() ? nullptr : internal_so.c_str(),
        "apktool_modern/lib/armeabi/libHeroesLore5.so",
        "/data/local/tmp/libHeroesLore5.so",
        nullptr
    };

    bool loaded = false;
    for (int i = 0; so_candidates[i] != nullptr; ++i) {
        if (access(so_candidates[i], R_OK) == 0) {
            loaded = g_elf_loader.load_so(so_candidates[i], g_native_engine, 0x40000000);
            if (loaded) break;
        }
    }

    if (loaded) {
        uint32_t make_inst_sym = g_elf_loader.find_symbol("_ZN18MIDASKernelManager12makeInstanceEv");
        if (make_inst_sym != 0) {
            g_km_instance = g_native_engine.call_function(make_inst_sym, {});
        }
        if (g_km_instance != 0) {
            // Set initial screen dimensions (400x240)
            uint32_t w = static_cast<uint32_t>(g_surface_w);
            uint32_t h = static_cast<uint32_t>(g_surface_h);
            g_native_engine.write_memory(g_km_instance + 0x624, &w, 4);
            g_native_engine.write_memory(g_km_instance + 0x628, &h, 4);

            // Initialize 2D screen GLES states
            uint32_t init2d_sym = g_elf_loader.find_symbol("_ZN18MIDASKernelManager12init2DScreenEv");
            if (init2d_sym != 0) {
                g_native_engine.call_function(init2d_sym, {g_km_instance});
            }

            // Create primaryScreen texture and set up vertex buffers
            uint32_t create_tex_sym = g_elf_loader.find_symbol("_ZN18MIDASKernelManager14createTexturesEv");
            if (create_tex_sym != 0) {
                g_native_engine.call_function(create_tex_sym, {g_km_instance});
            }

            g_engine_ready = true;
            g_last_frame_status = "Engine ready, primaryScreen texture created";
        } else {
            g_last_frame_status = "Singleton allocation failed";
        }
    } else {
        g_last_frame_status = "ELF binary not found or failed to load";
    }
}

JNIEXPORT void JNICALL
Java_co_kr_eamobile_hl5_runner_MainActivity_nativeOnSurfaceChanged(
    JNIEnv* /*env*/,
    jobject /*thiz*/,
    jint width,
    jint height) {
    // Runs on GLThread!
    g_surface_w = width;
    g_surface_h = height;

    if (g_engine_ready && g_km_instance != 0) {
        uint32_t w = static_cast<uint32_t>(width);
        uint32_t h = static_cast<uint32_t>(height);
        g_native_engine.write_memory(g_km_instance + 0x624, &w, 4);
        g_native_engine.write_memory(g_km_instance + 0x628, &h, 4);

        uint32_t init2d_sym = g_elf_loader.find_symbol("_ZN18MIDASKernelManager12init2DScreenEv");
        if (init2d_sym != 0) {
            g_native_engine.call_function(init2d_sym, {g_km_instance});
        }
    }
}

JNIEXPORT jstring JNICALL
Java_co_kr_eamobile_hl5_runner_MainActivity_nativeOnDrawFrame(
    JNIEnv* env,
    jobject /*thiz*/) {
    // Runs on GLThread with current EGLContext!
    g_frame_counter++;
    if (g_engine_ready && g_km_instance != 0) {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        // Execute recurring frame lifecycle:
        // 1. Process game logic and timers via nativeLoop if available
        uint32_t loop_sym = g_elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeLoop");
        uint32_t update_sym = g_elf_loader.find_symbol("_ZN18MIDASKernelManager12updateScreenEv");
        try {
            if (loop_sym != 0) {
                g_native_engine.call_function(loop_sym, {0, 0});
            } else if (update_sym != 0) {
                g_native_engine.call_function(update_sym, {g_km_instance});
            }
            
            // Query active state from CGame object ([0x40164368])
            uint32_t game_obj_var = 0x40164368;
            uint32_t game_obj = 0;
            g_native_engine.read_memory(game_obj_var, &game_obj, 4);
            if (game_obj >= 0x40000000) {
                g_game_instance = game_obj;
                uint32_t state_ptr = 0;
                g_native_engine.read_memory(game_obj + 0x27c, &state_ptr, 4);
                if (state_ptr >= 0x40000000) {
                    int8_t s_id = 0, ss_id = 0, p_id = 0;
                    g_native_engine.read_memory(state_ptr + 0xc, &s_id, 1);
                    g_native_engine.read_memory(state_ptr + 0xd, &ss_id, 1);
                    g_native_engine.read_memory(state_ptr + 0x14, &p_id, 1);
                    g_current_game_state = s_id;
                    g_current_game_substate = ss_id;
                    g_current_game_popup = p_id;
                }
            }
        } catch (const std::exception& e) {
            g_last_frame_status = std::string("Frame error: ") + e.what();
        }
    }

    const auto& diag = g_gles_bridge.diagnostics();
    char status_buf[512];
    snprintf(status_buf, sizeof(status_buf),
             "HL5 Frame #%llu | GLES Provider: %s\n"
             "Viewport: %dx%d | State: %d.%d (Pop: %d)\n"
             "DrawArrays: %u | TexBinds: %u (Tex: %u)\n"
             "Touch Rx/Tx: %u/%u | Last: t=%d raw=(%d,%d) game=(%d,%d)\n"
             "Ortho: [%.0f, %.0f, %.0f, %.0f]\n"
             "Status: %s",
             static_cast<unsigned long long>(g_frame_counter),
             g_gles_bridge.gles_provider_name(),
             g_surface_w, g_surface_h,
             g_current_game_state, g_current_game_substate, g_current_game_popup,
             diag.draw_arrays_calls,
             diag.bind_texture_calls,
             diag.last_bound_texture,
             g_touch_events_received.load(),
             g_touch_events_dispatched.load(),
             g_last_touch_type,
             g_last_touch_raw_x, g_last_touch_raw_y,
             g_last_touch_game_x, g_last_touch_game_y,
             diag.last_ortho_left, diag.last_ortho_right,
             diag.last_ortho_bottom, diag.last_ortho_top,
             g_last_frame_status.c_str());

    return env->NewStringUTF(status_buf);
}

JNIEXPORT void JNICALL
Java_co_kr_eamobile_hl5_runner_MainActivity_nativeOnTouchEvent(
    JNIEnv* /*env*/,
    jobject /*thiz*/,
    jint action,
    jfloat rawX,
    jfloat rawY,
    jint pointerId) {
    // Runs on Android UI Thread!
    g_touch_events_received++;

    int32_t raw_x_int = static_cast<int32_t>(rawX);
    int32_t raw_y_int = static_cast<int32_t>(rawY);
    g_last_touch_raw_x = raw_x_int;
    g_last_touch_raw_y = raw_y_int;

    // Convert Android MotionEvent action (0=DOWN, 1=UP, 2=MOVE) to HL5 event types:
    // 0x13 = Down / Pressed, 0x14 = Up / Released, 0x15 = Move / Dragged
    int32_t event_type = -1;
    if (action == 0) {
        event_type = 0x13;
    } else if (action == 1) {
        event_type = 0x14;
    } else if (action == 2) {
        event_type = 0x15;
    } else {
        return; // Ignore other actions (CANCEL, POINTER_DOWN, etc.)
    }
    g_last_touch_type = event_type;

    // Coordinate space mapping:
    // Target virtual resolution: 400x240 (or 800x480 screen with 2.0x magnification)
    // Clamp and transform from surface coordinates (g_surface_w x g_surface_h) to 400x240:
    int32_t game_x = 0;
    int32_t game_y = 0;
    if (g_surface_w > 0 && g_surface_h > 0) {
        game_x = (raw_x_int * 400) / g_surface_w;
        game_y = (raw_y_int * 240) / g_surface_h;
    }
    if (game_x < 0) game_x = 0;
    if (game_x > 399) game_x = 399;
    if (game_y < 0) game_y = 0;
    if (game_y > 239) game_y = 239;
    g_last_touch_game_x = game_x;
    g_last_touch_game_y = game_y;

    if (!g_engine_ready || g_km_instance == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_engine_mutex);
    uint32_t touch_sym = g_elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeTouchHandleEvent");
    if (touch_sym == 0) {
        touch_sym = g_elf_loader.find_symbol("_Z16nativeTouchHandlev"); // Fallback check
    }
    if (touch_sym != 0) {
        try {
            // JNI signature: nativeTouchHandleEvent(JNIEnv* env, jobject thiz, jint type, jint x, jint y, jint id)
            // In Unicorn guest execution:
            // R0 = jnienv (0), R1 = thiz (0), R2 = type, R3 = x
            // [sp + 0] = y, [sp + 4] = pointerId
            g_native_engine.call_function(touch_sym, {0, 0, static_cast<uint32_t>(event_type),
                                                      static_cast<uint32_t>(game_x),
                                                      static_cast<uint32_t>(game_y),
                                                      static_cast<uint32_t>(pointerId)});
            g_touch_events_dispatched++;
        } catch (const std::exception& e) {
            g_last_frame_status = std::string("Touch dispatch error: ") + e.what();
        }
    }
}


JNIEXPORT void JNICALL
Java_co_kr_eamobile_hl5_runner_MainActivity_nativeOnPause(
    JNIEnv* /*env*/,
    jobject /*thiz*/) {
    if (g_engine_ready && g_km_instance != 0) {
        uint32_t deinit_sym = g_elf_loader.find_symbol("_ZN18MIDASKernelManager14deinit2DScreenEv");
        if (deinit_sym != 0) {
            g_native_engine.call_function(deinit_sym, {g_km_instance});
        }
    }
}

JNIEXPORT void JNICALL
Java_co_kr_eamobile_hl5_runner_MainActivity_nativeOnResume(
    JNIEnv* /*env*/,
    jobject /*thiz*/) {
    if (g_engine_ready && g_km_instance != 0) {
        uint32_t init_sym = g_elf_loader.find_symbol("_ZN18MIDASKernelManager12init2DScreenEv");
        if (init_sym != 0) {
            g_native_engine.call_function(init_sym, {g_km_instance});
        }
    }
}

JNIEXPORT void JNICALL
Java_co_kr_eamobile_hl5_runner_MainActivity_nativeOnDestroy(
    JNIEnv* /*env*/,
    jobject /*thiz*/) {
    g_native_engine.close();
    g_engine_ready = false;
    g_km_instance = 0;
}

} // extern "C"
