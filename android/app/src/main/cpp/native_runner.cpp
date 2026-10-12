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
#include <csignal>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "HL5_NativeRunner"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do {} while(0)
#define LOGW(...) do {} while(0)
#define LOGE(...) do {} while(0)
#endif
namespace {
hl5::Arm32NativeEngine g_native_engine;
hl5::Arm32ElfLoader    g_elf_loader;
hl5::GlesBridge        g_gles_bridge;
hl5::VfsAdapter        g_vfs_adapter;
hl5::TypedHandleTable  g_handle_table;
std::mutex             g_engine_mutex;
uint32_t               g_km_instance = 0;
uint32_t               g_game_instance = 0;
uint32_t               g_jnienv32_ptr = 0;
uint32_t               g_javavm32_ptr = 0;
uint32_t               g_thiz_handle = 0;
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

// Unique asset name mapping (mirrors CletEntry.getUniqueNumberFromStrings / getUniqueAssetNameFromID)
std::unordered_map<std::string, int> g_name_to_id;
std::unordered_map<int, std::string> g_id_to_name;

void native_signal_handler(int sig, siginfo_t* info, void* /*context*/) {
    LOGE("FATAL NATIVE SIGNAL CAUGHT: signal=%d, si_code=%d, fault_addr=%p",
         sig, info ? info->si_code : 0, info ? info->si_addr : nullptr);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, nullptr);
    raise(sig);
}

void install_signal_handlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = native_signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
}

// Write a trampoline address into the 32-bit JNI vtable at the given byte offset
void register_jni_slot(uint32_t table_vaddr, uint32_t offset,
                        const std::string& name, hl5::HostHookFn fn) {
    uint32_t t_addr = g_native_engine.register_host_hook(name, std::move(fn));
    g_native_engine.write_memory(table_vaddr + offset, &t_addr, 4);
}

// Build the complete 32-bit JNIEnv* and JavaVM* vtables in guest memory,
// register all JNI function trampolines the guest calls during startup.
void setup_jni_bridge() {
    // --- JNINativeInterface table (256 function pointers × 4 bytes = 1024) ---
    const uint32_t table_size = 1024;
    uint32_t table_vaddr = g_native_engine.allocate_heap(table_size, 4096);
    // Zero-initialize
    std::vector<uint8_t> zeros(table_size, 0);
    g_native_engine.write_memory(table_vaddr, zeros.data(), table_size);

    // JNIEnv struct: pointer to table_vaddr
    g_jnienv32_ptr = g_native_engine.allocate_heap(8, 4);
    g_native_engine.write_memory(g_jnienv32_ptr, &table_vaddr, 4);

    // --- JNIInvokeInterface table (32 function pointers × 4 bytes = 128) ---
    uint32_t vm_table_vaddr = g_native_engine.allocate_heap(128, 4096);
    std::vector<uint8_t> vm_zeros(128, 0);
    g_native_engine.write_memory(vm_table_vaddr, vm_zeros.data(), 128);

    g_javavm32_ptr = g_native_engine.allocate_heap(8, 4);
    g_native_engine.write_memory(g_javavm32_ptr, &vm_table_vaddr, 4);

    // --- Register JNIEnv function trampolines ---

    // GetVersion (offset 0x10): returns JNI_VERSION_1_6
    register_jni_slot(table_vaddr, 0x10, "JNIEnv::GetVersion",
        [](const std::vector<uint32_t>&) -> uint32_t { return 0x00010006; });

    // FindClass (offset 0x18)
    register_jni_slot(table_vaddr, 0x18, "JNIEnv::FindClass",
        [](const std::vector<uint32_t>& args) -> uint32_t {
            std::string cls_name = g_native_engine.read_string(args[1]);
            return g_handle_table.allocate(hl5::HandleType::LOCAL_REF, nullptr, cls_name);
        });

    // NewGlobalRef (offset 0x54)
    register_jni_slot(table_vaddr, 0x54, "JNIEnv::NewGlobalRef",
        [](const std::vector<uint32_t>& args) -> uint32_t {
            uint32_t obj = args[1];
            if (obj == 0) return 0;
            auto* desc = g_handle_table.find_descriptor(obj);
            return g_handle_table.allocate(hl5::HandleType::GLOBAL_REF,
                desc ? desc->host_ptr : nullptr, desc ? desc->class_name : "");
        });

    // DeleteGlobalRef (offset 0x58)
    register_jni_slot(table_vaddr, 0x58, "JNIEnv::DeleteGlobalRef",
        [](const std::vector<uint32_t>& args) -> uint32_t {
            g_handle_table.delete_ref(args[1], true);
            return 0;
        });

    // DeleteLocalRef (offset 0x5C)
    register_jni_slot(table_vaddr, 0x5C, "JNIEnv::DeleteLocalRef",
        [](const std::vector<uint32_t>& args) -> uint32_t {
            g_handle_table.delete_ref(args[1], false);
            return 0;
        });

    // GetStaticMethodID (offset 0x1C4)
    register_jni_slot(table_vaddr, 0x1C4, "JNIEnv::GetStaticMethodID",
        [](const std::vector<uint32_t>& args) -> uint32_t {
            auto* cls_desc = g_handle_table.find_descriptor(args[1]);
            std::string cls_name = cls_desc ? cls_desc->class_name : "";
            std::string name = g_native_engine.read_string(args[2]);
            std::string sig = g_native_engine.read_string(args[3]);
            return g_handle_table.allocate(hl5::HandleType::METHOD_ID, nullptr, cls_name, sig + "|" + name);
        });

    // Helper lambda to resolve a method key and dispatch to built-in handlers
    auto dispatch_static = [](uint32_t mid_handle, const std::vector<uint32_t>& extra_args) -> std::pair<bool, uint32_t> {
        auto* desc = g_handle_table.find_descriptor(mid_handle);
        if (!desc) return {false, 0};
        // desc->signature format: "sig|name", desc->class_name = class
        std::string combined = desc->signature;
        size_t sep = combined.find('|');
        std::string sig = (sep != std::string::npos) ? combined.substr(0, sep) : "";
        std::string name = (sep != std::string::npos) ? combined.substr(sep + 1) : "";
        std::string cls = desc->class_name;

        // Built-in Java method stubs:
        if (cls == "co/kr/eamobile/CletEntry") {
            if (name == "getUniqueNumberFromStrings" && extra_args.size() >= 1) {
                // Resolve string handle → lookup/assign unique ID
                auto* str_desc = g_handle_table.find_descriptor(extra_args[0]);
                std::string asset_name = str_desc ? str_desc->class_name : "";
                auto it = g_name_to_id.find(asset_name);
                if (it == g_name_to_id.end()) {
                    int idx = static_cast<int>(g_name_to_id.size()) + 1;
                    g_name_to_id[asset_name] = idx;
                    g_id_to_name[idx] = asset_name;
                    return {true, static_cast<uint32_t>(idx)};
                }
                return {true, static_cast<uint32_t>(it->second)};
            }
            if (name == "getUniqueAssetNameFromID" && extra_args.size() >= 1) {
                int id = static_cast<int>(extra_args[0]);
                auto it = g_id_to_name.find(id);
                std::string result = (it != g_id_to_name.end()) ? it->second : "";
                uint32_t str_ptr = g_native_engine.write_string(result);
                uint32_t h = g_handle_table.allocate(hl5::HandleType::LOCAL_REF, nullptr, result);
                // Also write string pointer in guest for GetStringUTFChars
                return {true, h};
            }
            if (name == "getMin") {
                // Return IMEI-like string
                uint32_t str_ptr = g_native_engine.write_string("01000000000");
                uint32_t h = g_handle_table.allocate(hl5::HandleType::LOCAL_REF,
                    reinterpret_cast<void*>(static_cast<uintptr_t>(str_ptr)), "01000000000");
                return {true, h};
            }
            if (name == "freeMemory") return {true, 64 * 1024 * 1024};
            if (name == "totalMemory") return {true, 128 * 1024 * 1024};
            if (name == "isNetworkAvailable") return {true, 0};
        }
        return {false, 0};
    };

    // CallStaticObjectMethod (offset 0x1D4)
    register_jni_slot(table_vaddr, 0x1D4, "JNIEnv::CallStaticObjectMethod",
        [dispatch_static](const std::vector<uint32_t>& args) -> uint32_t {
            auto [handled, val] = dispatch_static(args[2], {args.begin() + 3, args.end()});
            return handled ? val : 0;
        });

    // CallStaticObjectMethodV (offset 0x1CC) — va_list variant used by nativeInitKernel
    register_jni_slot(table_vaddr, 0x1CC, "JNIEnv::CallStaticObjectMethodV",
        [dispatch_static](const std::vector<uint32_t>& args) -> uint32_t {
            // args[3] = va_list pointer; unpack up to 8 words
            std::vector<uint32_t> va;
            uint32_t va_ptr = args[3];
            if (va_ptr >= 0x10000 && va_ptr <= 0x7FFFFFFF) {
                uint32_t buf[8] = {0};
                g_native_engine.read_memory(va_ptr, buf, sizeof(buf));
                for (int i = 0; i < 8; ++i) va.push_back(buf[i]);
            }
            auto [handled, val] = dispatch_static(args[2], va);
            return handled ? val : 0;
        });

    // CallStaticIntMethod (offset 0x1EC)
    register_jni_slot(table_vaddr, 0x1EC, "JNIEnv::CallStaticIntMethod",
        [dispatch_static](const std::vector<uint32_t>& args) -> uint32_t {
            auto [handled, val] = dispatch_static(args[2], {args.begin() + 3, args.end()});
            return handled ? val : 0;
        });

    // CallStaticIntMethodV (offset 0x208) — va_list variant
    register_jni_slot(table_vaddr, 0x208, "JNIEnv::CallStaticIntMethodV",
        [dispatch_static](const std::vector<uint32_t>& args) -> uint32_t {
            std::vector<uint32_t> va;
            uint32_t va_ptr = args[3];
            if (va_ptr >= 0x10000 && va_ptr <= 0x7FFFFFFF) {
                uint32_t buf[8] = {0};
                g_native_engine.read_memory(va_ptr, buf, sizeof(buf));
                for (int i = 0; i < 8; ++i) va.push_back(buf[i]);
            }
            auto [handled, val] = dispatch_static(args[2], va);
            return handled ? val : 0;
        });

    // CallStaticLongMethodV (offset 0x214)
    register_jni_slot(table_vaddr, 0x214, "JNIEnv::CallStaticLongMethodV",
        [dispatch_static](const std::vector<uint32_t>& args) -> uint32_t {
            std::vector<uint32_t> va;
            uint32_t va_ptr = args[3];
            if (va_ptr >= 0x10000 && va_ptr <= 0x7FFFFFFF) {
                uint32_t buf[8] = {0};
                g_native_engine.read_memory(va_ptr, buf, sizeof(buf));
                for (int i = 0; i < 8; ++i) va.push_back(buf[i]);
            }
            auto [handled, val] = dispatch_static(args[2], va);
            return handled ? val : 0;
        });

    // CallStaticVoidMethod (offset 0x204)
    register_jni_slot(table_vaddr, 0x204, "JNIEnv::CallStaticVoidMethod",
        [dispatch_static](const std::vector<uint32_t>& args) -> uint32_t {
            dispatch_static(args[2], {args.begin() + 3, args.end()});
            return 0;
        });

    // CallStaticVoidMethodV (offset 0x238) — va_list variant
    register_jni_slot(table_vaddr, 0x238, "JNIEnv::CallStaticVoidMethodV",
        [dispatch_static](const std::vector<uint32_t>& args) -> uint32_t {
            std::vector<uint32_t> va;
            uint32_t va_ptr = args[3];
            if (va_ptr >= 0x10000 && va_ptr <= 0x7FFFFFFF) {
                uint32_t buf[8] = {0};
                g_native_engine.read_memory(va_ptr, buf, sizeof(buf));
                for (int i = 0; i < 8; ++i) va.push_back(buf[i]);
            }
            dispatch_static(args[2], va);
            return 0;
        });

    // NewStringUTF (offset 0x294)
    register_jni_slot(table_vaddr, 0x294, "JNIEnv::NewStringUTF",
        [](const std::vector<uint32_t>& args) -> uint32_t {
            std::string s = g_native_engine.read_string(args[1]);
            // Store the guest string pointer as host_ptr for GetStringUTFChars
            return g_handle_table.allocate(hl5::HandleType::LOCAL_REF,
                reinterpret_cast<void*>(static_cast<uintptr_t>(args[1])), s);
        });

    // Secondary NewStringUTF slot at 0x29C (observed in some call paths)
    register_jni_slot(table_vaddr, 0x29C, "JNIEnv::NewStringUTF_29C",
        [](const std::vector<uint32_t>& args) -> uint32_t {
            std::string s = g_native_engine.read_string(args[1]);
            return g_handle_table.allocate(hl5::HandleType::LOCAL_REF,
                reinterpret_cast<void*>(static_cast<uintptr_t>(args[1])), s);
        });

    // GetStringUTFChars (offset 0x2A4)
    register_jni_slot(table_vaddr, 0x2A4, "JNIEnv::GetStringUTFChars",
        [](const std::vector<uint32_t>& args) -> uint32_t {
            uint32_t str_handle = args[1];
            auto* desc = g_handle_table.find_descriptor(str_handle);
            if (!desc) return 0;
            // If host_ptr holds original guest string address, return it
            uintptr_t guest_ptr = reinterpret_cast<uintptr_t>(desc->host_ptr);
            if (guest_ptr >= 0x10000 && guest_ptr < 0x80000000) {
                return static_cast<uint32_t>(guest_ptr);
            }
            // Otherwise write class_name (the string content) to guest memory
            return g_native_engine.write_string(desc->class_name);
        });

    // ReleaseStringUTFChars (offset 0x2A8) — no-op
    register_jni_slot(table_vaddr, 0x2A8, "JNIEnv::ReleaseStringUTFChars",
        [](const std::vector<uint32_t>&) -> uint32_t { return 0; });

    // GetJavaVM (offset 0x36C)
    register_jni_slot(table_vaddr, 0x36C, "JNIEnv::GetJavaVM",
        [](const std::vector<uint32_t>& args) -> uint32_t {
            uint32_t vm_out = args[1];
            if (vm_out != 0) {
                g_native_engine.write_memory(vm_out, &g_javavm32_ptr, 4);
            }
            return 0; // JNI_OK
        });

    // GetFieldID (offset 0x178)
    register_jni_slot(table_vaddr, 0x178, "JNIEnv::GetFieldID",
        [](const std::vector<uint32_t>& args) -> uint32_t {
            auto* cls_desc = g_handle_table.find_descriptor(args[1]);
            std::string cls_name = cls_desc ? cls_desc->class_name : "";
            std::string name = g_native_engine.read_string(args[2]);
            std::string sig = g_native_engine.read_string(args[3]);
            return g_handle_table.allocate(hl5::HandleType::FIELD_ID, nullptr, cls_name, name);
        });

    // GetIntField (offset 0x190) — returns fd descriptor for FileDescriptor objects
    register_jni_slot(table_vaddr, 0x190, "JNIEnv::GetIntField",
        [](const std::vector<uint32_t>& args) -> uint32_t {
            auto* obj_desc = g_handle_table.find_descriptor(args[1]);
            if (!obj_desc) return 0;
            // If class_name starts with "fd:", return the numeric fd value
            const std::string& cn = obj_desc->class_name;
            if (cn.substr(0, 3) == "fd:") {
                return static_cast<uint32_t>(std::stoi(cn.substr(3)));
            }
            return 0;
        });

    // --- JavaVM::GetEnv (vm_table offset 0x18) ---
    register_jni_slot(vm_table_vaddr, 0x18, "JavaVM::GetEnv",
        [](const std::vector<uint32_t>& args) -> uint32_t {
            if (args[1] != 0) {
                g_native_engine.write_memory(args[1], &g_jnienv32_ptr, 4);
            }
            return 0; // JNI_OK
        });

    LOGI("JNI bridge vtable created: JNIEnv*=0x%08x, JavaVM*=0x%08x", g_jnienv32_ptr, g_javavm32_ptr);
}
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
    install_signal_handlers();
    LOGI("[MILESTONE 1/9] nativeOnSurfaceCreated entered on GLThread");
    g_frame_counter = 0;
    g_engine_ready = false;

    // 1. Initialize GLES Bridge with host driver
    LOGI("[MILESTONE 2/9] Initializing EGL/GLES 1.1 Bridge");
    bool gles_ok = g_gles_bridge.initialize();
    g_gles_bridge.reset_diagnostics();
    LOGI("[MILESTONE 2/9 RESULT] GLES Bridge result: %d (provider: %s)", gles_ok, g_gles_bridge.gles_provider_name());

    // 2. Initialize ARM32 Unicorn CPU Engine
    LOGI("[MILESTONE 3/9] Initializing ARM32 Unicorn CPU Engine");
    if (!g_native_engine.initialize()) {
        LOGE("[MILESTONE 3/9 FAIL] Engine initialization failed on GLThread");
        g_last_frame_status = "[FAIL] Engine initialization failed on GLThread";
        return;
    }
    LOGI("[MILESTONE 3/9 PASS] Native ARM32 Unicorn CPU Engine initialized successfully on GLThread");

    // 3. Register GLES hooks + VFS hooks
    g_gles_bridge.register_with_engine(g_native_engine);
    g_vfs_adapter.register_with_engine(g_native_engine);

    // 4. Load libHeroesLore5.so
    LOGI("[MILESTONE 4/9] Locating and loading ARM32 guest ELF libHeroesLore5.so");
    const char* int_chars = internalPathStr ? env->GetStringUTFChars(internalPathStr, nullptr) : nullptr;
    std::string int_dir_str = (int_chars && int_chars[0] != '\0') ? int_chars : "";
    if (int_chars) env->ReleaseStringUTFChars(internalPathStr, int_chars);

    const char* ext_chars = externalPathStr ? env->GetStringUTFChars(externalPathStr, nullptr) : nullptr;
    std::string ext_dir_str = (ext_chars && ext_chars[0] != '\0') ? ext_chars : "";
    if (ext_chars) env->ReleaseStringUTFChars(externalPathStr, ext_chars);

    std::string internal_so = int_dir_str.empty() ? "" : (int_dir_str + "/libHeroesLore5.so");
    std::string int_dir = int_dir_str.empty() ? "/data/local/tmp" : int_dir_str;
    std::string ext_dir = ext_dir_str.empty() ? int_dir : ext_dir_str;

    const char* so_candidates[] = {
        internal_so.empty() ? nullptr : internal_so.c_str(),
        "apktool_modern/lib/armeabi/libHeroesLore5.so",
        "/data/local/tmp/libHeroesLore5.so",
        nullptr
    };

    bool loaded = false;
    for (int i = 0; so_candidates[i] != nullptr; ++i) {
        if (access(so_candidates[i], R_OK) == 0) {
            LOGI("Found candidate ELF at: %s, loading...", so_candidates[i]);
            loaded = g_elf_loader.load_so(so_candidates[i], g_native_engine, 0x40000000);
            if (loaded) {
                LOGI("[MILESTONE 4/9 PASS] Successfully loaded and relocated ELF: %s", so_candidates[i]);
                break;
            } else {
                LOGE("Failed to load ELF: %s", so_candidates[i]);
            }
        }
    }

    if (!loaded) {
        LOGE("ELF binary not found or failed to load");
        g_last_frame_status = "ELF binary not found or failed to load";
        return;
    }

    // 5. Build JNI bridge vtable in guest memory
    LOGI("[MILESTONE 5/9] Setting up JNI bridge vtable");
    setup_jni_bridge();

    // 6. Execute authentic boot sequence (mirrors CletEntry.smali startup)
    LOGI("[MILESTONE 6/9] Executing authentic guest boot sequence");
    try {
        // Stage 1: nativeInitKernel — creates AndroidService singleton, caches Java method IDs
        g_handle_table.push_local_frame();
        g_thiz_handle = g_handle_table.allocate(hl5::HandleType::LOCAL_REF, nullptr, "CletEntry_instance");
        uint32_t init_kernel_sym = g_elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeInitKernel");
        if (init_kernel_sym != 0) {
            LOGI("[BOOT 1/7] nativeInitKernel at 0x%08x", init_kernel_sym);
            g_native_engine.call_function(init_kernel_sym, {g_jnienv32_ptr, g_thiz_handle}, 10000000);
            LOGI("[BOOT 1/7] nativeInitKernel complete");
        } else {
            LOGW("[BOOT 1/7] nativeInitKernel symbol not found — skipping");
        }
        g_handle_table.pop_local_frame();

        // Stage 2: nativeInitDisplay — configures screen dimensions, color depth, magnification
        g_handle_table.push_local_frame();
        g_thiz_handle = g_handle_table.allocate(hl5::HandleType::LOCAL_REF, nullptr, "CletEntry_instance");
        uint32_t init_display_sym = g_elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeInitDisplay");
        if (init_display_sym != 0) {
            // Magnification = 2.0f packed as uint32
            float mag = 2.0f;
            uint32_t mag_bits = 0;
            memcpy(&mag_bits, &mag, sizeof(float));

            LOGI("[BOOT 2/7] nativeInitDisplay at 0x%08x (800x480 → 400x240 @ 2.0x mag)", init_display_sym);
            g_native_engine.call_function(init_display_sym, {
                g_jnienv32_ptr, g_thiz_handle,
                800, 480,        // screenW, screenH
                5, 6, 5, 0,      // R, G, B, A bits (RGB565)
                400, 240, 0, 0,  // wipiW, wipiH, marginX, marginY
                0, mag_bits, 0, 0, // wipiColor, magnification, filterType, viewMode
                0, 0             // useSublayer, sublayerPixel
            }, 10000000);
            LOGI("[BOOT 2/7] nativeInitDisplay complete");
        } else {
            LOGW("[BOOT 2/7] nativeInitDisplay symbol not found — using manual init");
            uint32_t make_sym = g_elf_loader.find_symbol("_ZN18MIDASKernelManager12makeInstanceEv");
            if (make_sym) g_km_instance = g_native_engine.call_function(make_sym, {});
            if (g_km_instance != 0) {
                uint32_t w = 400, h = 240;
                g_native_engine.write_memory(g_km_instance + 0x624, &w, 4);
                g_native_engine.write_memory(g_km_instance + 0x628, &h, 4);
                uint32_t init2d = g_elf_loader.find_symbol("_ZN18MIDASKernelManager12init2DScreenEv");
                if (init2d) g_native_engine.call_function(init2d, {g_km_instance});
                uint32_t tex = g_elf_loader.find_symbol("_ZN18MIDASKernelManager14createTexturesEv");
                if (tex) g_native_engine.call_function(tex, {g_km_instance});
            }
        }
        g_handle_table.pop_local_frame();

        // Get km_instance after nativeInitDisplay (it calls makeInstance internally)
        uint32_t get_inst_sym = g_elf_loader.find_symbol("_ZN18MIDASKernelManager11getInstanceEv");
        if (get_inst_sym != 0) {
            g_km_instance = g_native_engine.call_function(get_inst_sym, {});
            LOGI("MIDASKernelManager singleton at 0x%08x", g_km_instance);
        }

        // Stage 3: nativeInitFileStorage — sets internal/external storage paths
        g_handle_table.push_local_frame();
        g_thiz_handle = g_handle_table.allocate(hl5::HandleType::LOCAL_REF, nullptr, "CletEntry_instance");
        uint32_t init_fs_sym = g_elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeInitFileStorage");
        if (init_fs_sym != 0) {
            uint32_t int_str_ptr = g_native_engine.write_string(int_dir);
            uint32_t ext_str_ptr = g_native_engine.write_string(ext_dir);
            uint32_t int_str_h = g_handle_table.allocate(hl5::HandleType::LOCAL_REF,
                reinterpret_cast<void*>(static_cast<uintptr_t>(int_str_ptr)), int_dir);
            uint32_t ext_str_h = g_handle_table.allocate(hl5::HandleType::LOCAL_REF,
                reinterpret_cast<void*>(static_cast<uintptr_t>(ext_str_ptr)), ext_dir);

            LOGI("[BOOT 3/7] nativeInitFileStorage at 0x%08x (int=%s, ext=%s)", init_fs_sym, int_dir.c_str(), ext_dir.c_str());
            g_native_engine.call_function(init_fs_sym, {g_jnienv32_ptr, g_thiz_handle, int_str_h, ext_str_h}, 5000000);
            LOGI("[BOOT 3/7] nativeInitFileStorage complete");
        }
        g_handle_table.pop_local_frame();

        // Stage 4: nativeInitVFS — opens and registers data.vfs.mp3 with VFS adapter
        g_handle_table.push_local_frame();
        g_thiz_handle = g_handle_table.allocate(hl5::HandleType::LOCAL_REF, nullptr, "CletEntry_instance");
        uint32_t init_vfs_sym = g_elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeInitVFS");
        if (init_vfs_sym != 0) {
            std::string vfs_paths[] = {
                int_dir + "/data.vfs.mp3",
                ext_dir + "/data.vfs.mp3",
                "apktool_modern/assets/data.vfs.mp3",
                "/data/local/tmp/data.vfs.mp3"
            };
            int vfs_host_fd = -1;
            off_t vfs_len = 0;
            for (const auto& path : vfs_paths) {
                int fd = open(path.c_str(), O_RDONLY);
                if (fd >= 0) {
                    struct stat st;
                    if (fstat(fd, &st) == 0) {
                        vfs_len = st.st_size;
                        vfs_host_fd = fd;
                        LOGI("[BOOT 4/7] Found data.vfs.mp3 at %s (%ld bytes)", path.c_str(), (long)vfs_len);
                        break;
                    }
                    close(fd);
                }
            }

            if (vfs_host_fd >= 0) {
                uint32_t guest_fd = g_vfs_adapter.register_asset(vfs_host_fd, 0, vfs_len);
                LOGI("[BOOT 4/7] VFS registered: guest_fd=0x%x, len=%ld", guest_fd, (long)vfs_len);

                std::string fd_class = "fd:" + std::to_string(guest_fd);
                uint32_t fd_handle = g_handle_table.allocate(hl5::HandleType::LOCAL_REF, nullptr, fd_class);

                uint32_t len_low = static_cast<uint32_t>(vfs_len & 0xFFFFFFFF);
                uint32_t len_high = static_cast<uint32_t>((vfs_len >> 32) & 0xFFFFFFFF);

                LOGI("[BOOT 4/7] nativeInitVFS at 0x%08x", init_vfs_sym);
                g_native_engine.call_function(init_vfs_sym, {
                    g_jnienv32_ptr, g_thiz_handle,
                    fd_handle, 0,
                    0, 0,
                    len_low, len_high
                }, 10000000);
                LOGI("[BOOT 4/7] nativeInitVFS complete");
            } else {
                LOGW("[BOOT 4/7] data.vfs.mp3 not found — VFS not initialized");
            }
        }
        g_handle_table.pop_local_frame();

        // Stage 5: nativeInitAudioConfiguration
        g_handle_table.push_local_frame();
        g_thiz_handle = g_handle_table.allocate(hl5::HandleType::LOCAL_REF, nullptr, "CletEntry_instance");
        uint32_t init_audio_sym = g_elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeInitAudioConfiguration");
        if (init_audio_sym != 0) {
            LOGI("[BOOT 5/7] nativeInitAudioConfiguration at 0x%08x", init_audio_sym);
            g_native_engine.call_function(init_audio_sym, {g_jnienv32_ptr, g_thiz_handle, 202400}, 5000000);
            LOGI("[BOOT 5/7] nativeInitAudioConfiguration complete");
        }
        g_handle_table.pop_local_frame();

        // Stage 6: nativeStartApp — creates CGame, enters main menu
        g_handle_table.push_local_frame();
        g_thiz_handle = g_handle_table.allocate(hl5::HandleType::LOCAL_REF, nullptr, "CletEntry_instance");
        uint32_t start_app_sym = g_elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeStartApp");
        if (start_app_sym != 0) {
            LOGI("[BOOT 6/7] nativeStartApp at 0x%08x — entering main game initialization", start_app_sym);
            g_native_engine.call_function(start_app_sym, {g_jnienv32_ptr, g_thiz_handle}, 0);
            LOGI("[BOOT 6/7] nativeStartApp complete");
        }
        g_handle_table.pop_local_frame();

        // Stage 7: Read CGame pointer and verify state
        uint32_t game_obj = 0;
        g_native_engine.read_memory(0x40164368, &game_obj, 4);
        if (game_obj >= 0x40000000) {
            g_game_instance = game_obj;
            LOGI("[BOOT 7/7] CGame object at 0x%08x — game world initialized", game_obj);
        } else {
            LOGW("[BOOT 7/7] CGame pointer not set (0x%08x) — game may not have fully initialized", game_obj);
        }

        g_engine_ready = true;
        g_last_frame_status = "Boot complete: JNI bridge + authentic init sequence";
        LOGI("[MILESTONE 9/9] Full authentic boot sequence complete, ready for frame rendering");

    } catch (const std::exception& e) {
        LOGE("Boot sequence failed: %s", e.what());
        g_last_frame_status = std::string("Boot failed: ") + e.what();
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
    if (g_frame_counter == 1) {
        LOGI("[MILESTONE 6/9] First game loop frame triggered on GLThread");
    }
    if (g_engine_ready && g_km_instance != 0) {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        // Execute recurring frame lifecycle:
        // 1. Process game logic and timers via nativeLoop if available
        uint32_t loop_sym = g_elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeLoop");
        uint32_t update_sym = g_elf_loader.find_symbol("_ZN18MIDASKernelManager12updateScreenEv");
        try {
            if (loop_sym != 0) {
                g_native_engine.call_function(loop_sym, {g_jnienv32_ptr, g_thiz_handle});
            } else if (update_sym != 0) {
                g_native_engine.call_function(update_sym, {g_km_instance});
            }
            if (g_frame_counter == 1) {
                LOGI("[MILESTONE 7/9] Successfully executed first guest game-loop tick");
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
                    // Authentic Offline Hardening:
                    // 1. Cold boot fast-fail: State 0 (CheckEvent) normally stalls 60s on netConnect(9).
                    //    Fast-fail cold boot directly to State 2 (DrawLogo) so game opens into logo/title in <2s.
                    // 2. Mark legacy carrier SMS flag [game_obj + 42] = 'N' (0x4E) to skip SMS consent loop.
                    // 3. Mark cloud save check flag [game_obj + 142] = 1 (0x01) so Title Screen and Data Management
                    //    bypass DSPSock::DSP_Connect() and transition directly to Main Menu (State 4) & local storage.
                    uint8_t sms_flag = 0, server_check_flag = 0;
                    g_native_engine.read_memory(game_obj + 42, &sms_flag, 1);
                    g_native_engine.read_memory(game_obj + 142, &server_check_flag, 1);

                    if (sms_flag == 0) {
                        uint8_t flag = 'N';
                        g_native_engine.write_memory(game_obj + 42, &flag, 1);
                    }
                    if (server_check_flag != 1) {
                        uint8_t flag = 1;
                        g_native_engine.write_memory(game_obj + 142, &flag, 1);
                    }

                    if (s_id == 0) {
                        // Cold boot fast-fail: bypass 60s dead-server CheckEvent hang
                        uint32_t load_logo_sym = g_elf_loader.find_symbol("_ZN13StateMainMenu14LoadLogoSpriteEa");
                        uint32_t set_state_sym = g_elf_loader.find_symbol("_ZN13StateMainMenu8SetStateEa");
                        if (load_logo_sym != 0 && set_state_sym != 0) {
                            g_native_engine.call_function(load_logo_sym, {state_ptr, 0});
                            g_native_engine.call_function(set_state_sym, {state_ptr, 2});
                            LOGI("[OFFLINE_HARDENING] Fast-failed cold boot State 0 (CheckEvent) -> State 2 (DrawLogo)");
                        }
                    } else if (s_id == 2 && ss_id == 1) {
                        // If SMS consent prompt (SubState 1) was somehow entered, cleanly recover to Logo Animation (SubState 3)
                        uint32_t load_logo_sym = g_elf_loader.find_symbol("_ZN13StateMainMenu14LoadLogoSpriteEa");
                        uint32_t set_sub_sym = g_elf_loader.find_symbol("_ZN13StateMainMenu11SetSubStateEa");
                        if (load_logo_sym != 0 && set_sub_sym != 0) {
                            g_native_engine.call_function(load_logo_sym, {state_ptr, 1});
                            g_native_engine.call_function(set_sub_sym, {state_ptr, 3});
                            LOGI("[OFFLINE_HARDENING] Recovered from SMS consent prompt (SubState 1) -> Logo Animation (SubState 3)");
                        }
                    } else if (s_id == 3) {
                        // In State 3 (Title Screen), ensure [game_obj + 142] = 1 is locked so touches advance to State 4 Main Menu
                        uint8_t flag = 1;
                        g_native_engine.write_memory(game_obj + 142, &flag, 1);
                    }
                }
            }
        } catch (const std::exception& e) {
            g_last_frame_status = std::string("Frame error: ") + e.what();
        }
    }

    const auto& diag = g_gles_bridge.diagnostics();
    if (g_frame_counter == 1) {
        LOGI("[MILESTONE 8/9] First frame rendered: DrawArrays=%u, Ortho=[%.0f, %.0f, %.0f, %.0f]",
             diag.draw_arrays_calls, diag.last_ortho_left, diag.last_ortho_right,
             diag.last_ortho_bottom, diag.last_ortho_top);
    }
    if (diag.draw_arrays_calls > 0 && g_frame_counter <= 5) {
        LOGI("[MILESTONE 9/9] Non-empty rendered frame confirmed (total draws: %u)", diag.draw_arrays_calls);
    }
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
            g_native_engine.call_function(touch_sym, {g_jnienv32_ptr, g_thiz_handle,
                                                      static_cast<uint32_t>(event_type),
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
    LOGI("nativeOnPause invoked from UI thread");
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    if (g_engine_ready && g_km_instance != 0) {
        try {
            uint32_t deinit_sym = g_elf_loader.find_symbol("_ZN18MIDASKernelManager14deinit2DScreenEv");
            if (deinit_sym != 0) {
                g_native_engine.call_function(deinit_sym, {g_km_instance});
            }
        } catch (const std::exception& e) {
            LOGW("nativeOnPause deinit exception: %s", e.what());
        }
    }
}

JNIEXPORT void JNICALL
Java_co_kr_eamobile_hl5_runner_MainActivity_nativeOnResume(
    JNIEnv* /*env*/,
    jobject /*thiz*/) {
    LOGI("nativeOnResume invoked from UI thread");
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    if (g_engine_ready && g_km_instance != 0) {
        try {
            uint32_t init_sym = g_elf_loader.find_symbol("_ZN18MIDASKernelManager12init2DScreenEv");
            if (init_sym != 0) {
                g_native_engine.call_function(init_sym, {g_km_instance});
            }
        } catch (const std::exception& e) {
            LOGW("nativeOnResume init exception: %s", e.what());
        }
    }
}

JNIEXPORT void JNICALL
Java_co_kr_eamobile_hl5_runner_MainActivity_nativeOnDestroy(
    JNIEnv* /*env*/,
    jobject /*thiz*/) {
    LOGI("nativeOnDestroy invoked from UI thread");
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    try {
        g_native_engine.close();
    } catch (...) {}
    g_engine_ready = false;
    g_km_instance = 0;
}
} // extern "C"
