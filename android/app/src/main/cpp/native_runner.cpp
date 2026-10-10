#include <jni.h>
#include <string>
#include <sstream>
#include "jni_bridge_core.h"
#include "gles_bridge.h"
#include "vfs_adapter.h"
#include "arm32_native_engine.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <cerrno>
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

} // extern "C"
