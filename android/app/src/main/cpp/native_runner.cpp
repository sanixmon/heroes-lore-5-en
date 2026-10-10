#include <jni.h>
#include <string>
#include <sstream>
#include "jni_bridge_core.h"
#include "gles_bridge.h"

extern "C" {

JNIEXPORT jstring JNICALL
Java_co_kr_eamobile_hl5_runner_MainActivity_nativeRunDiagnosticTests(JNIEnv* env, jobject thiz) {
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

    return env->NewStringUTF(oss.str().c_str());
}

} // extern "C"
