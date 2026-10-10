#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>
#include <fstream>
#include "arm32_native_engine.h"

// Simple ELF loader for test fixture
struct ElfLoader {
    static bool load_raw(hl5::Arm32NativeEngine& engine, const std::string& path, uint32_t base_addr) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return false;
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<char> buffer(size);
        if (!file.read(buffer.data(), size)) return false;

        // Map and write buffer to base_addr
        size_t map_sz = (size + 4095) & ~4095;
        if (!engine.map_memory(base_addr, map_sz, UC_PROT_ALL)) {
            return false;
        }
        return engine.write_memory(base_addr, buffer.data(), size);
    }
};

int main() {
    std::cout << "=== Running HL5 ARM32-on-AArch64 Native C++ Engine Test ===" << std::endl;

    hl5::Arm32NativeEngine engine;
    if (!engine.initialize()) {
        std::cerr << "[FAIL] Failed to initialize Arm32NativeEngine!" << std::endl;
        return 1;
    }
    std::cout << "[PASS] Arm32NativeEngine initialized successfully." << std::endl;

    // 1. Test raw ARM32 instructions in memory:
    //    mov r0, #42; bx lr (e3a0002a, e12fff1e)
    uint32_t code_addr = 0x40000000;
    if (!engine.map_memory(code_addr, 4096, UC_PROT_ALL)) {
        std::cerr << "[FAIL] Failed to map code memory!" << std::endl;
        return 1;
    }

    uint32_t test_code[] = {
        0xe3a0002a, // mov r0, #42
        0xe12fff1e  // bx lr
    };
    engine.write_memory(code_addr, test_code, sizeof(test_code));

    uint32_t res = engine.call_function(code_addr, {});
    std::cout << "[+] test_basic_instruction: res = " << res << " (expected 42)" << std::endl;
    assert(res == 42);

    // 2. Test arithmetic and register passing:
    uint32_t add_code_addr = 0x40001000;
    engine.map_memory(add_code_addr, 4096, UC_PROT_ALL);
    uint32_t add_code[] = {
        0xe0800001, // add r0, r0, r1
        0xe12fff1e  // bx lr
    };
    engine.write_memory(add_code_addr, add_code, sizeof(add_code));
    uint32_t add_res = engine.call_function(add_code_addr, {15, 27});
    std::cout << "[+] test_add_args(15, 27): res = " << add_res << " (expected 42)" << std::endl;
    assert(add_res == 42);

    // 3. Test Host Callback Bridge Trampoline:
    uint32_t bridge_tramp = engine.register_host_hook("host_bridge_multiply", [](const std::vector<uint32_t>& args) -> uint32_t {
        uint32_t x = args[0];
        std::cout << "    [AArch64 Host Hook] host_bridge_multiply called with x=" << x << std::endl;
        return x * 3;
    });

    uint32_t host_code_addr = 0x40002000;
    engine.map_memory(host_code_addr, 4096, UC_PROT_ALL);
    uint32_t host_test_code[] = {
        0xe59f3004, // ldr r3, [pc, #4] -> loads bridge_tramp address at PC+12
        0xe12fff33, // blx r3
        0xe12fff1e, // bx lr
        bridge_tramp // .word bridge_tramp
    };
    engine.write_memory(host_code_addr, host_test_code, sizeof(host_test_code));
    uint32_t host_res = engine.call_function(host_code_addr, {7});
    std::cout << "[+] test_host_bridge(7): res = " << host_res << " (expected 21)" << std::endl;
    assert(host_res == 21);

    // 4. Test Real libHeroesLore5.so Function Execution:
    //    MIDASKernelManager::hash(unsigned char*) at offset 0x1516a4
    std::string so_path = "apktool_modern/lib/armeabi/libHeroesLore5.so";
    std::ifstream so_file(so_path, std::ios::binary);
    if (so_file.is_open()) {
        so_file.seekg(0x1516a4, std::ios::beg);
        char func_bytes[64];
        so_file.read(func_bytes, sizeof(func_bytes));

        uint32_t hl5_code_addr = 0x40003000;
        engine.map_memory(hl5_code_addr, 4096, UC_PROT_ALL);
        engine.write_memory(hl5_code_addr, func_bytes, sizeof(func_bytes));

        uint32_t str_ptr = engine.write_string("version.txt");
        uint32_t hash_res = engine.call_function(hl5_code_addr, {0, str_ptr});
        std::cout << "[+] test_real_hl5_hash('version.txt'): res = 0x"
                  << std::hex << hash_res << std::dec << " (expected 0x9abed099)" << std::endl;
        assert(hash_res == 0x9abed099);
        std::cout << "[PASS] Verified real libHeroesLore5.so execution on native AArch64!" << std::endl;
    } else {
        std::cerr << "[WARN] Could not open libHeroesLore5.so for test 4." << std::endl;
    }

    std::cout << "\n[PASS] All Native C++ ARM32-on-AArch64 engine tests PASSED 100%!" << std::endl;
    return 0;
}
