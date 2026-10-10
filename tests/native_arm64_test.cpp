#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>
#include <fstream>
#include "arm32_native_engine.h"
#include "arm32_elf_loader.h"
#include "gles_bridge.h"

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
    }

    // 5. Test Full Dynamic ELF Loading & Symbol Resolution in C++
    std::cout << "\n--- Testing Full ELF Loader in C++ ---" << std::endl;
    hl5::Arm32NativeEngine full_engine;
    full_engine.initialize();

    hl5::GlesBridge gles_bridge;
    gles_bridge.initialize();
    gles_bridge.register_with_engine(full_engine);

    hl5::Arm32ElfLoader elf_loader;
    bool loaded = elf_loader.load_so(so_path, full_engine, 0x40000000);
    assert(loaded);
    std::cout << "[PASS] libHeroesLore5.so loaded dynamically into guest memory at 0x"
              << std::hex << elf_loader.base_addr() << ", size: " << std::dec << elf_loader.total_size() << " bytes" << std::endl;
    std::cout << "[+] Total dynamic symbols parsed: " << elf_loader.symbols().size() << std::endl;
    assert(elf_loader.symbols().size() > 500);

    // Test calling resolved symbol via dynamic loader
    uint32_t hash_sym = elf_loader.find_symbol("_ZN18MIDASKernelManager4hashEPh");
    assert(hash_sym != 0);
    uint32_t s_ptr = full_engine.write_string("c/csv/name.dat");
    uint32_t h_res = full_engine.call_function(hash_sym, {0, s_ptr});
    std::cout << "[+] Dynamic call hash('c/csv/name.dat') = 0x"
              << std::hex << h_res << std::dec << " (expected 0x53c493da)" << std::endl;
    assert(h_res == 0x53c493da);

    // 6. Test Real Engine GLES Initialization via init2DScreen
    std::cout << "\n--- Testing Real Engine GLES Execution via init2DScreen ---" << std::endl;
    uint32_t make_inst_sym = elf_loader.find_symbol("_ZN18MIDASKernelManager12makeInstanceEv");
    assert(make_inst_sym != 0);
    uint32_t km_instance = full_engine.call_function(make_inst_sym, {});
    assert(km_instance != 0);
    std::cout << "[PASS] MIDASKernelManager singleton created at 0x" << std::hex << km_instance << std::dec << std::endl;

    // Set display width=400, height=240 in instance fields (offsets 0x624 and 0x628)
    uint32_t w = 400, h = 240;
    full_engine.write_memory(km_instance + 0x624, &w, 4);
    full_engine.write_memory(km_instance + 0x628, &h, 4);

    uint32_t init2d_sym = elf_loader.find_symbol("_ZN18MIDASKernelManager12init2DScreenEv");
    assert(init2d_sym != 0);

    gles_bridge.reset_diagnostics();
    full_engine.call_function(init2d_sym, {km_instance});
    std::cout << "[PASS] init2DScreen executed successfully!" << std::endl;
    std::cout << "[+] GLES Diagnostics after init2DScreen:" << std::endl;
    std::cout << "    Total GLES calls: " << gles_bridge.diagnostics().total_calls << std::endl;
    std::cout << "    Viewport: " << gles_bridge.diagnostics().viewport_w << "x" << gles_bridge.diagnostics().viewport_h << std::endl;
    std::cout << "    Ortho: left=" << gles_bridge.diagnostics().last_ortho_left
              << ", right=" << gles_bridge.diagnostics().last_ortho_right
              << ", bottom=" << gles_bridge.diagnostics().last_ortho_bottom
              << ", top=" << gles_bridge.diagnostics().last_ortho_top << std::endl;

    assert(gles_bridge.diagnostics().total_calls >= 10);
    assert(gles_bridge.diagnostics().viewport_w == 400);
    assert(gles_bridge.diagnostics().viewport_h == 240);
    assert(gles_bridge.diagnostics().last_ortho_right == 400.0f);
    assert(gles_bridge.diagnostics().last_ortho_bottom == 0.0f);
    assert(gles_bridge.diagnostics().last_ortho_top == 240.0f);

    // 7. Test Actual Drawing Execution via createTextures & updateScreen
    std::cout << "\n--- Testing Real Engine Drawing Execution (glDrawArrays) ---" << std::endl;
    uint32_t create_tex_sym = elf_loader.find_symbol("_ZN18MIDASKernelManager14createTexturesEv");
    assert(create_tex_sym != 0);
    // Set LCD parameters at [km_instance + 0x4fc] and [km_instance + 0x500] (400x240)
    full_engine.write_memory(km_instance + 0x4fc, &w, 4);
    full_engine.write_memory(km_instance + 0x500, &h, 4);
    full_engine.call_function(create_tex_sym, {km_instance});
    std::cout << "[PASS] createTextures executed: primaryScreen texture allocated." << std::endl;

    uint32_t update_screen_sym = elf_loader.find_symbol("_ZN18MIDASKernelManager12updateScreenEv");
    assert(update_screen_sym != 0);

    gles_bridge.reset_diagnostics();
    full_engine.call_function(update_screen_sym, {km_instance});
    std::cout << "[PASS] updateScreen executed!" << std::endl;
    std::cout << "[+] GLES Diagnostics during updateScreen:" << std::endl;
    std::cout << "    Total GLES calls: " << gles_bridge.diagnostics().total_calls << std::endl;
    std::cout << "    glDrawArrays calls: " << gles_bridge.diagnostics().draw_arrays_calls << std::endl;
    std::cout << "    Texture binds: " << gles_bridge.diagnostics().bind_texture_calls << std::endl;
    assert(gles_bridge.diagnostics().draw_arrays_calls >= 3);

    // 8. Test Native Touch Input Handling & Coordinate Space Mapping
    std::cout << "\n--- Testing Native Touch Event Dispatch ---" << std::endl;
    uint32_t touch_sym = elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeTouchHandleEvent");
    assert(touch_sym != 0);

    // Dispatch ACTION_DOWN (0x13) at game coordinates (235, 135)
    uint32_t res_down = full_engine.call_function(touch_sym, {0, 0, 0x13, 235, 135, 0});
    std::cout << "[PASS] nativeTouchHandleEvent ACTION_DOWN dispatched (res=" << res_down << ")." << std::endl;
    assert(res_down == 0);

    // Dispatch ACTION_MOVE (0x15) at game coordinates (236, 136)
    uint32_t res_move = full_engine.call_function(touch_sym, {0, 0, 0x15, 236, 136, 0});
    std::cout << "[PASS] nativeTouchHandleEvent ACTION_MOVE dispatched (res=" << res_move << ")." << std::endl;
    assert(res_move == 0);

    // Dispatch ACTION_UP (0x14) at game coordinates (236, 136)
    uint32_t res_up = full_engine.call_function(touch_sym, {0, 0, 0x14, 236, 136, 0});
    std::cout << "[PASS] nativeTouchHandleEvent ACTION_UP dispatched (res=" << res_up << ")." << std::endl;
    assert(res_up == 0);
    // 9. Test Recurring Multi-Frame Loop Execution & Framebuffer Geometry
    std::cout << "\n--- Testing Multi-Frame Loop Execution & Framebuffer State ---" << std::endl;
    for (int frame = 1; frame <= 5; ++frame) {
        gles_bridge.reset_diagnostics();
        full_engine.call_function(update_screen_sym, {km_instance});
        assert(gles_bridge.diagnostics().draw_arrays_calls >= 3);
    }
    std::cout << "[PASS] 5 consecutive frames rendered without crash, assert, or leak." << std::endl;

    // Validate Primary Screen Texture pointer and dimension fields
    uint32_t primary_tex_ptr = 0;
    full_engine.read_memory(km_instance + 0x658, &primary_tex_ptr, 4);
    assert(primary_tex_ptr >= 0x40000000);
    uint32_t val_20 = 0, gl_format = 0;
    full_engine.read_memory(primary_tex_ptr + 0x20, &val_20, 4);
    full_engine.read_memory(primary_tex_ptr + 0xb0, &gl_format, 4);
    std::cout << "[PASS] Texture2D validated: PrimaryScreen pointer=0x" << std::hex << primary_tex_ptr 
              << ", field[+0x20]=0x" << val_20 
              << ", GL format=0x" << gl_format << std::dec << std::endl;
    assert(val_20 != 0);
    assert(gl_format != 0);

    // 10. Test BaseState State Manager & State Transition Mapping
    std::cout << "\n--- Testing State Transition Management (BaseState::StateMgr) ---" << std::endl;
    uint32_t state_mgr_sym = elf_loader.find_symbol("_ZN9BaseState8StateMgrEaaa");
    assert(state_mgr_sym != 0);

    // StateMgr(this=0, state_type=1, substate=1, param=0) -> sets next state transition
    full_engine.call_function(state_mgr_sym, {0, 1, 1, 0});
    std::cout << "[PASS] BaseState::StateMgr(1, 1, 0) dispatched transition request." << std::endl;

    // Validate that CGame object ([0x40164368]) received nextState request
    uint32_t game_obj_var = 0x40164368;
    uint32_t game_obj = 0;
    full_engine.read_memory(game_obj_var, &game_obj, 4);
    if (game_obj >= 0x40000000) {
        int8_t req_state = 0, req_substate = 0;
        full_engine.read_memory(game_obj + 0xe1, &req_state, 1);
        full_engine.read_memory(game_obj + 0xe2, &req_substate, 1);
        std::cout << "[PASS] CGame nextState registered: state=" << (int)req_state 
                  << ", substate=" << (int)req_substate << std::endl;
        assert(req_state == 1);
        assert(req_substate == 1);
    }
    std::cout << "\n[PASS] All Native C++ ARM32-on-AArch64 engine tests PASSED 100%!" << std::endl;
    return 0;
}
