#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <unordered_map>
#include <sstream>
#include "arm32_native_engine.h"
#include <unordered_set>

static uint64_t g_sim_time_ms = 10000;

#include "arm32_elf_loader.h"
#include "gles_bridge.h"
#include "vfs_adapter.h"
#include "jni_bridge_core.h"

static hl5::Arm32NativeEngine g_native_engine;
static hl5::Arm32ElfLoader    g_elf_loader;
static hl5::GlesBridge        g_gles_bridge;
static hl5::VfsAdapter        g_vfs_adapter;
static hl5::TypedHandleTable  g_handle_table;

static uint32_t g_jnienv32_ptr = 0;
static uint32_t g_javavm32_ptr = 0;
static uint32_t g_thiz_handle = 0;
static uint32_t g_km_instance = 0;

static std::unordered_map<std::string, int> g_name_to_id;
static std::unordered_map<int, std::string> g_id_to_name;

static void register_jni_slot(uint32_t table_vaddr, uint32_t offset,
                              const std::string& name, hl5::HostHookFn fn) {
    uint32_t t_addr = g_native_engine.register_host_hook(name, std::move(fn));
    g_native_engine.write_memory(table_vaddr + offset, &t_addr, 4);
}

static void setup_jni_bridge() {
    const uint32_t table_size = 1024;
    uint32_t table_vaddr = g_native_engine.allocate_heap(table_size, 4096);
    std::vector<uint8_t> zeros(table_size, 0);
    g_native_engine.write_memory(table_vaddr, zeros.data(), table_size);

    g_jnienv32_ptr = g_native_engine.allocate_heap(8, 4);
    g_native_engine.write_memory(g_jnienv32_ptr, &table_vaddr, 4);

    uint32_t vm_table_vaddr = g_native_engine.allocate_heap(128, 4096);
    std::vector<uint8_t> vm_zeros(128, 0);
    g_native_engine.write_memory(vm_table_vaddr, vm_zeros.data(), 128);

    g_javavm32_ptr = g_native_engine.allocate_heap(8, 4);
    g_native_engine.write_memory(g_javavm32_ptr, &vm_table_vaddr, 4);

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

    auto dispatch_static = [](uint32_t mid_handle, const std::vector<uint32_t>& extra_args) -> std::pair<bool, uint32_t> {
        auto* desc = g_handle_table.find_descriptor(mid_handle);
        if (!desc) return {false, 0};
        std::string combined = desc->signature;
        size_t sep = combined.find('|');
        std::string sig = (sep != std::string::npos) ? combined.substr(0, sep) : "";
        std::string name = (sep != std::string::npos) ? combined.substr(sep + 1) : "";
        std::string cls = desc->class_name;

        if (cls == "co/kr/eamobile/CletEntry") {
            if (name == "getUniqueNumberFromStrings" && extra_args.size() >= 1) {
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
                uint32_t h = g_handle_table.allocate(hl5::HandleType::LOCAL_REF,
                    reinterpret_cast<void*>(static_cast<uintptr_t>(str_ptr)), result);
                return {true, h};
            }
            if (name == "getMin") {
                uint32_t h = g_handle_table.allocate(hl5::HandleType::LOCAL_REF, nullptr, "01000000000");
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

    // CallStaticObjectMethodV (offset 0x1CC)
    register_jni_slot(table_vaddr, 0x1CC, "JNIEnv::CallStaticObjectMethodV",
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

    // CallStaticIntMethod (offset 0x1EC)
    register_jni_slot(table_vaddr, 0x1EC, "JNIEnv::CallStaticIntMethod",
        [dispatch_static](const std::vector<uint32_t>& args) -> uint32_t {
            auto [handled, val] = dispatch_static(args[2], {args.begin() + 3, args.end()});
            return handled ? val : 0;
        });

    // CallStaticIntMethodV (offset 0x208)
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

    // CallStaticVoidMethodV (offset 0x238)
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
            return g_handle_table.allocate(hl5::HandleType::LOCAL_REF,
                reinterpret_cast<void*>(static_cast<uintptr_t>(args[1])), s);
        });

    // Secondary NewStringUTF slot at 0x29C
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
            uintptr_t guest_ptr = reinterpret_cast<uintptr_t>(desc->host_ptr);
            if (guest_ptr >= 0x10000 && guest_ptr < 0x80000000) {
                return static_cast<uint32_t>(guest_ptr);
            }
            return g_native_engine.write_string(desc->class_name);
        });

    // ReleaseStringUTFChars (offset 0x2A8)
    register_jni_slot(table_vaddr, 0x2A8, "JNIEnv::ReleaseStringUTFChars",
        [](const std::vector<uint32_t>&) -> uint32_t { return 0; });

    // GetJavaVM (offset 0x36C)
    register_jni_slot(table_vaddr, 0x36C, "JNIEnv::GetJavaVM",
        [](const std::vector<uint32_t>& args) -> uint32_t {
            uint32_t vm_out = args[1];
            if (vm_out != 0) {
                g_native_engine.write_memory(vm_out, &g_javavm32_ptr, 4);
            }
            return 0;
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

    // GetIntField (offset 0x190)
    register_jni_slot(table_vaddr, 0x190, "JNIEnv::GetIntField",
        [](const std::vector<uint32_t>& args) -> uint32_t {
            auto* obj_desc = g_handle_table.find_descriptor(args[1]);
            if (!obj_desc) return 0;
            const std::string& cn = obj_desc->class_name;
            if (cn.substr(0, 3) == "fd:") {
                return static_cast<uint32_t>(std::stoi(cn.substr(3)));
            }
            return 0;
        });

    // JavaVM::GetEnv (vm_table offset 0x18)
    register_jni_slot(vm_table_vaddr, 0x18, "JavaVM::GetEnv",
        [](const std::vector<uint32_t>& args) -> uint32_t {
            if (args[1] != 0) {
                g_native_engine.write_memory(args[1], &g_jnienv32_ptr, 4);
            }
            return 0;
        });
}

static bool on_mem_unmapped(uc_engine* uc, uc_mem_type type, uint64_t address, int size, int64_t value, void* user_data) {
    uint32_t pc = 0, lr = 0, sp = 0;
    uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    uc_reg_read(uc, UC_ARM_REG_R1, &r1);
    uc_reg_read(uc, UC_ARM_REG_R2, &r2);
    uc_reg_read(uc, UC_ARM_REG_R3, &r3);
    uc_reg_read(uc, UC_ARM_REG_R4, &r4);
    uc_reg_read(uc, UC_ARM_REG_R5, &r5);
    uc_reg_read(uc, UC_ARM_REG_R6, &r6);
    uc_reg_read(uc, UC_ARM_REG_R7, &r7);
    std::cout << "\n[UNMAPPED MEMORY ACCESS]"
              << "\n  Type: " << type << " (1=READ, 2=WRITE, 3=FETCH)"
              << "\n  Address: 0x" << std::hex << address << " (size=" << std::dec << size << ")"
              << "\n  PC: 0x" << std::hex << pc << " (rel: 0x" << (pc - 0x40000000) << ")"
              << "\n  LR: 0x" << std::hex << lr << " (rel: 0x" << (lr - 0x40000000) << ")"
              << "\n  SP: 0x" << std::hex << sp
              << "\n  R0: 0x" << std::hex << r0 << "  R1: 0x" << r1 << "  R2: 0x" << r2 << "  R3: 0x" << r3
              << "\n  R4: 0x" << std::hex << r4 << "  R5: 0x" << r5 << "  R6: 0x" << r6 << "  R7: 0x" << r7
              << std::dec << std::endl;
    return false;
}

int main() {
    std::cout << "=== Running HL5 Full Boot Simulation Test ===" << std::endl;
    if (!g_native_engine.initialize()) {
        std::cerr << "[FAIL] Failed to init engine" << std::endl;
        return 1;
    }
    uc_hook hook_unmapped;
    uc_hook_add(g_native_engine.uc_, &hook_unmapped, UC_HOOK_MEM_UNMAPPED,
                reinterpret_cast<void*>(on_mem_unmapped), nullptr, 1, 0);
    g_native_engine.register_host_hook("vsnprintf", [&engine = g_native_engine](const std::vector<uint32_t>& args) -> uint32_t {
        uint32_t buf_ptr = args[0];
        uint32_t size = args[1];
        uint32_t fmt_ptr = args[2];
        uint32_t va_ptr = args[3];
        std::string fmt = engine.read_string(fmt_ptr);

        std::string out;
        out.reserve(256);
        uint32_t cur_va = va_ptr;

        for (size_t i = 0; i < fmt.size(); ) {
            if (fmt[i] == '%') {
                if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
                    out += '%';
                    i += 2;
                    continue;
                }
                // Parse format specifier
                size_t j = i + 1;
                // Flags / width / precision
                while (j < fmt.size() && (isdigit(fmt[j]) || fmt[j] == '-' || fmt[j] == '+' || fmt[j] == ' ' || fmt[j] == '#' || fmt[j] == '.' || fmt[j] == 'l' || fmt[j] == 'h')) {
                    j++;
                }
                if (j < fmt.size()) {
                    char conv = fmt[j];
                    std::string spec = fmt.substr(i, j - i + 1);
                    uint32_t val = 0;
                    engine.read_memory(cur_va, &val, 4);
                    cur_va += 4;

                    char formatted[128] = {0};
                    if (conv == 's') {
                        std::string s = engine.read_string(val);
                        out += s;
                    } else if (conv == 'd' || conv == 'i') {
                        snprintf(formatted, sizeof(formatted), spec.c_str(), static_cast<int32_t>(val));
                        out += formatted;
                    } else if (conv == 'u' || conv == 'x' || conv == 'X') {
                        snprintf(formatted, sizeof(formatted), spec.c_str(), val);
                        out += formatted;
                    } else if (conv == 'c') {
                        out += static_cast<char>(val & 0xFF);
                    } else {
                        out += spec;
                    }
                    i = j + 1;
                    continue;
                }
            }
            out += fmt[i++];
        }

        if (size > 0) {
            size_t copy_len = std::min(out.size(), static_cast<size_t>(size - 1));
            engine.write_memory(buf_ptr, out.c_str(), copy_len);
            char null_byte = 0;
            engine.write_memory(buf_ptr + copy_len, &null_byte, 1);
        }
        return static_cast<uint32_t>(out.size());
    });
    g_native_engine.register_host_hook("ftime", [&engine = g_native_engine](const std::vector<uint32_t>& args) -> uint32_t {
        uint32_t tp_ptr = args[0];
        g_sim_time_ms += 50;
        uint32_t sec = static_cast<uint32_t>(g_sim_time_ms / 1000);
        uint16_t millitm = static_cast<uint16_t>(g_sim_time_ms % 1000);
        int16_t timezone = 0;
        int16_t dstflag = 0;
        engine.write_memory(tp_ptr, &sec, 4);
        engine.write_memory(tp_ptr + 4, &millitm, 2);
        engine.write_memory(tp_ptr + 6, &timezone, 2);
        engine.write_memory(tp_ptr + 8, &dstflag, 2);
        return 0;
    });
    g_gles_bridge.initialize();
    g_gles_bridge.register_with_engine(g_native_engine);
    g_vfs_adapter.register_with_engine(g_native_engine);

    std::string so_path = "apktool_modern/lib/armeabi/libHeroesLore5.so";
    if (!g_elf_loader.load_so(so_path, g_native_engine, 0x40000000)) {
        std::cerr << "[FAIL] Failed to load ELF" << std::endl;
        return 1;
    }
    std::cout << "[PASS] ELF loaded at 0x" << std::hex << g_elf_loader.base_addr() << std::dec << std::endl;

    setup_jni_bridge();
    std::cout << "[PASS] JNI bridge setup complete: JNIEnv*=0x" << std::hex << g_jnienv32_ptr
              << ", JavaVM*=0x" << g_javavm32_ptr << std::dec << std::endl;

    // Stage 1: nativeInitKernel
    g_handle_table.push_local_frame();
    g_thiz_handle = g_handle_table.allocate(hl5::HandleType::LOCAL_REF, nullptr, "CletEntry_instance");
    uint32_t init_kernel_sym = g_elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeInitKernel");
    assert(init_kernel_sym != 0);
    std::cout << "[+] Calling nativeInitKernel at 0x" << std::hex << init_kernel_sym << std::dec << std::endl;
    g_native_engine.call_function(init_kernel_sym, {g_jnienv32_ptr, g_thiz_handle}, 10000000);
    std::cout << "[PASS] Stage 1 nativeInitKernel complete" << std::endl;
    g_handle_table.pop_local_frame();

    // Stage 2: nativeInitDisplay
    g_handle_table.push_local_frame();
    g_thiz_handle = g_handle_table.allocate(hl5::HandleType::LOCAL_REF, nullptr, "CletEntry_instance");
    uint32_t init_display_sym = g_elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeInitDisplay");
    assert(init_display_sym != 0);
    float mag = 2.0f;
    uint32_t mag_bits = 0;
    memcpy(&mag_bits, &mag, sizeof(float));
    std::cout << "[+] Calling nativeInitDisplay at 0x" << std::hex << init_display_sym << std::dec << std::endl;
    uint32_t disp_res = g_native_engine.call_function(init_display_sym, {
        g_jnienv32_ptr, g_thiz_handle,
        800, 480,
        5, 6, 5, 0,
        400, 240, 0, 0,
        0, mag_bits, 0, 0,
        0, 0
    }, 10000000);
    std::cout << "[PASS] Stage 2 nativeInitDisplay complete, autoMarginY=" << disp_res << std::endl;
    g_handle_table.pop_local_frame();

    uint32_t get_inst_sym = g_elf_loader.find_symbol("_ZN18MIDASKernelManager11getInstanceEv");
    assert(get_inst_sym != 0);
    g_km_instance = g_native_engine.call_function(get_inst_sym, {});
    std::cout << "[+] MIDASKernelManager singleton at 0x" << std::hex << g_km_instance << std::dec << std::endl;
    assert(g_km_instance != 0);

    // Stage 3: nativeInitFileStorage
    std::string int_dir = "/tmp/hl5_int";
    std::string ext_dir = "/tmp/hl5_ext";
    g_handle_table.push_local_frame();
    g_thiz_handle = g_handle_table.allocate(hl5::HandleType::LOCAL_REF, nullptr, "CletEntry_instance");
    uint32_t init_fs_sym = g_elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeInitFileStorage");
    assert(init_fs_sym != 0);
    uint32_t int_str_ptr = g_native_engine.write_string(int_dir);
    uint32_t ext_str_ptr = g_native_engine.write_string(ext_dir);
    uint32_t int_str_h = g_handle_table.allocate(hl5::HandleType::LOCAL_REF,
        reinterpret_cast<void*>(static_cast<uintptr_t>(int_str_ptr)), int_dir);
    uint32_t ext_str_h = g_handle_table.allocate(hl5::HandleType::LOCAL_REF,
        reinterpret_cast<void*>(static_cast<uintptr_t>(ext_str_ptr)), ext_dir);
    g_native_engine.call_function(init_fs_sym, {g_jnienv32_ptr, g_thiz_handle, int_str_h, ext_str_h}, 5000000);
    std::cout << "[PASS] Stage 3 nativeInitFileStorage complete" << std::endl;
    g_handle_table.pop_local_frame();

    // Stage 4: nativeInitVFS
    std::string vfs_path = "apktool_modern/assets/data.vfs.mp3";
    int vfs_fd = open(vfs_path.c_str(), O_RDONLY);
    assert(vfs_fd >= 0);
    struct stat st;
    fstat(vfs_fd, &st);
    off_t vfs_len = st.st_size;
    std::cout << "[+] data.vfs.mp3 size = " << vfs_len << " bytes" << std::endl;

    uint32_t guest_fd = g_vfs_adapter.register_asset(vfs_fd, 0, vfs_len);
    std::string fd_class = "fd:" + std::to_string(guest_fd);

    g_handle_table.push_local_frame();
    g_thiz_handle = g_handle_table.allocate(hl5::HandleType::LOCAL_REF, nullptr, "CletEntry_instance");
    uint32_t fd_handle = g_handle_table.allocate(hl5::HandleType::LOCAL_REF, nullptr, fd_class);

    uint32_t init_vfs_sym = g_elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeInitVFS");
    assert(init_vfs_sym != 0);
    uint32_t len_low = static_cast<uint32_t>(vfs_len & 0xFFFFFFFF);
    uint32_t len_high = static_cast<uint32_t>((vfs_len >> 32) & 0xFFFFFFFF);
    g_native_engine.call_function(init_vfs_sym, {
        g_jnienv32_ptr, g_thiz_handle,
        fd_handle, 0,
        0, 0,
        len_low, len_high
    }, 10000000);
    std::cout << "[PASS] Stage 4 nativeInitVFS complete" << std::endl;
    g_handle_table.pop_local_frame();

    // Verify VFS reading version.txt
    uint32_t get_asset_sz_sym = g_elf_loader.find_symbol("_ZN18MIDASKernelManager19getAssetSizeFromVFSEPKcPm");
    std::cout << "[+] getAssetSizeFromVFS symbol at 0x" << std::hex << get_asset_sz_sym << std::dec << std::endl;
    if (get_asset_sz_sym != 0) {
        uint32_t ver_str_ptr = g_native_engine.write_string("version.txt");
        uint32_t out_ptr = g_native_engine.allocate_heap(8, 4);
        uint32_t sz_res = g_native_engine.call_function(get_asset_sz_sym, {g_km_instance, ver_str_ptr, out_ptr}, 5000000);
        uint32_t off_res = 0;
        g_native_engine.read_memory(out_ptr, &off_res, 4);
        std::cout << "[+] getAssetSizeFromVFS('version.txt') = " << sz_res << ", offset=" << off_res << std::endl;

        uint32_t bld_str_ptr = g_native_engine.write_string("c/csv/buildup.dat");
        uint32_t bld_sz = g_native_engine.call_function(get_asset_sz_sym, {g_km_instance, bld_str_ptr, out_ptr}, 5000000);
        uint32_t bld_off = 0;
        g_native_engine.read_memory(out_ptr, &bld_off, 4);
        std::cout << "[+] getAssetSizeFromVFS('c/csv/buildup.dat') = " << bld_sz << ", offset=" << bld_off << std::endl;
    }

    // Stage 5: nativeInitAudioConfiguration
    g_handle_table.push_local_frame();
    g_thiz_handle = g_handle_table.allocate(hl5::HandleType::LOCAL_REF, nullptr, "CletEntry_instance");
    uint32_t init_audio_sym = g_elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeInitAudioConfiguration");
    assert(init_audio_sym != 0);
    g_native_engine.call_function(init_audio_sym, {g_jnienv32_ptr, g_thiz_handle, 202400}, 5000000);
    std::cout << "[PASS] Stage 5 nativeInitAudioConfiguration complete" << std::endl;
    g_handle_table.pop_local_frame();

    // Stage 6: nativeStartApp
    g_handle_table.push_local_frame();
    g_thiz_handle = g_handle_table.allocate(hl5::HandleType::LOCAL_REF, nullptr, "CletEntry_instance");
    uint32_t start_app_sym = g_elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeStartApp");
    assert(start_app_sym != 0);
    std::cout << "[+] Calling nativeStartApp at 0x" << std::hex << start_app_sym << std::dec << std::endl;
    g_native_engine.call_function(start_app_sym, {g_jnienv32_ptr, g_thiz_handle}, 0);
    std::cout << "[PASS] Stage 6 nativeStartApp complete" << std::endl;
    g_handle_table.pop_local_frame();

    // Read CGame pointer
    uint32_t game_obj = 0;
    g_native_engine.read_memory(0x40164368, &game_obj, 4);
    std::cout << "[+] CGame pointer at [0x40164368] = 0x" << std::hex << game_obj << std::dec << std::endl;
    assert(game_obj >= 0x40000000);

    uint32_t state_ptr = 0;
    g_native_engine.read_memory(game_obj + 0x27c, &state_ptr, 4);
    std::cout << "[+] Current state pointer before loop = 0x" << std::hex << state_ptr << std::dec << std::endl;
    // Run nativeLoop frames
    std::cout << "[+] Tracing Frame 1..." << std::endl;
    // Check game_obj fields before frame 1
    uint32_t f_e1 = 0, f_e2 = 0;
    g_native_engine.read_memory(game_obj + 0xe1, &f_e1, 1);
    g_native_engine.read_memory(game_obj + 0xe2, &f_e2, 1);
    std::cout << "[+] Before Frame 1: next_state=" << (int)(int8_t)f_e1 << "." << (int)(int8_t)f_e2 << std::endl;
    uint32_t t_head = 0;
    g_native_engine.read_memory(g_km_instance + 16, &t_head, 4);
    std::cout << "[+] timer_head at [km + 16] = 0x" << std::hex << t_head << std::dec << std::endl;
    if (t_head >= 0x40000000) {
        uint32_t r8 = t_head - 8;
        uint32_t words[8] = {0};
        g_native_engine.read_memory(r8, words, sizeof(words));
        for (int i = 0; i < 8; ++i) {
            std::cout << "  r8[" << i << "] = 0x" << std::hex << words[i] << std::dec << std::endl;
        }
    }
    uint32_t cur_time_sym = g_elf_loader.find_symbol("_ZN18MIDASKernelManager11currentTimeEv");
    if (cur_time_sym) {
        uint32_t t_low = g_native_engine.call_function(cur_time_sym, {g_km_instance});
        uint32_t t_high = 0;
        uc_reg_read(g_native_engine.uc_, UC_ARM_REG_R1, &t_high);
        std::cout << "[+] currentTime() = 0x" << std::hex << t_high << ":" << t_low << std::dec << std::endl;
    }
    uint32_t loop_sym = g_elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeLoop");
    uint32_t touch_sym = g_elf_loader.find_symbol("Java_co_kr_eamobile_CletEntry_nativeTouchHandleEvent");
    assert(loop_sym != 0);

    std::cout << "[+] Running frames through nativeLoop..." << std::endl;
    int8_t max_state = -1;
    for (int frame = 1; frame <= 180; ++frame) {
        if (frame == 18 || frame == 37 || (frame > 120 && frame < 130 && frame % 5 == 0) || frame == 160) {
            g_sim_time_ms += 65000;
        }
        if (frame == 19 || frame == 38 || frame == 127 || frame == 161) {
            // touch event
            g_native_engine.call_function(touch_sym, {g_jnienv32_ptr, g_thiz_handle, 0x13, 200, 140, 0});
            g_native_engine.call_function(touch_sym, {g_jnienv32_ptr, g_thiz_handle, 0x14, 200, 140, 0});
        }

        uint32_t cur_state = 0;
        g_native_engine.read_memory(game_obj + 0x27c, &cur_state, 4);
        if (cur_state >= 0x40000000) {
            uint32_t vt = 0;
            g_native_engine.read_memory(cur_state, &vt, 4);
            int8_t s = 0, ss = 0;
            g_native_engine.read_memory(cur_state + 0xc, &s, 1);
            g_native_engine.read_memory(cur_state + 0xd, &ss, 1);
            if (s > max_state) max_state = s;
            if (vt == 0x40163ae0) { // StateMainMenu
                if (s == 2 && ss == 1) {
                    g_native_engine.call_function(touch_sym, {g_jnienv32_ptr, g_thiz_handle, 0x13, 260, 160, 0});
                    g_native_engine.call_function(touch_sym, {g_jnienv32_ptr, g_thiz_handle, 0x14, 260, 160, 0});
                } else if (s == 2 && ss == 4) {
                    g_native_engine.call_function(touch_sym, {g_jnienv32_ptr, g_thiz_handle, 0x13, 200, 120, 0});
                    g_native_engine.call_function(touch_sym, {g_jnienv32_ptr, g_thiz_handle, 0x14, 200, 120, 0});
                } else if (s == 3 && ss == 1) {
                    g_native_engine.call_function(touch_sym, {g_jnienv32_ptr, g_thiz_handle, 0x13, 200, 120, 0});
                    g_native_engine.call_function(touch_sym, {g_jnienv32_ptr, g_thiz_handle, 0x14, 200, 120, 0});
                }
            }
        }

        g_native_engine.call_function(loop_sym, {g_jnienv32_ptr, g_thiz_handle}, 10000000);

        if (frame % 10 == 0 || frame == 1) {
            uint32_t cur_st = 0;
            g_native_engine.read_memory(game_obj + 0x27c, &cur_st, 4);
            int8_t s = -1, ss = -1;
            if (cur_st >= 0x40000000) {
                g_native_engine.read_memory(cur_st + 0xc, &s, 1);
                g_native_engine.read_memory(cur_st + 0xd, &ss, 1);
            }
            std::cout << "  Frame " << frame << ": State = " << (int)s << "." << (int)ss << std::endl;
            if (s >= 3) {
                std::cout << "[+] Reached Title Screen / Main Menu (State " << (int)s << "." << (int)ss << ")!" << std::endl;
                if (frame >= 170 || s == 4) break;
            }
        }
    }

    uint32_t cur_st = 0;
    g_native_engine.read_memory(game_obj + 0x27c, &cur_st, 4);
    assert(cur_st >= 0x40000000);
    int8_t final_s = -1, final_ss = -1;
    g_native_engine.read_memory(cur_st + 0xc, &final_s, 1);
    g_native_engine.read_memory(cur_st + 0xd, &final_ss, 1);
    std::cout << "[+] Final verified state: State = " << (int)final_s << "." << (int)final_ss << std::endl;
    assert(max_state >= 3 || final_s >= 3);
    // Verify framebuffer pixel rendering
    uint32_t graphic_obj = 0;
    g_native_engine.read_memory(0x4016fcc8, &graphic_obj, 4);
    assert(graphic_obj >= 0x40000000);
    uint32_t fb_ptr = 0;
    g_native_engine.read_memory(graphic_obj, &fb_ptr, 4);
    assert(fb_ptr >= 0x40000000);

    std::vector<uint16_t> fb(400 * 240);
    g_native_engine.read_memory(fb_ptr, fb.data(), fb.size() * 2);
    std::unordered_set<uint16_t> unique_colors(fb.begin(), fb.end());
    std::cout << "[+] FrameBuffer=0x" << std::hex << fb_ptr << std::dec
              << ", Unique Colors=" << unique_colors.size() << std::endl;
    assert(unique_colors.size() >= 100);

    std::cout << "\n[PASS] Reached Original Title Screen (State 3) in Native C++ on AArch64!" << std::endl;
    return 0;
}
#if !defined(__ANDROID__)
#include <errno.h>
#include <setjmp.h>
extern "C" {
int* __errno(void) { return &errno; }
int sigsetjmp_compat(sigjmp_buf env, int savesigs) { return __sigsetjmp(env, savesigs); }
__asm__(".global sigsetjmp\n.type sigsetjmp, %function\nsigsetjmp:\nb __sigsetjmp\n");
}
#endif
