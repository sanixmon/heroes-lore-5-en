#ifndef HL5_ARM32_NATIVE_ENGINE_H
#define HL5_ARM32_NATIVE_ENGINE_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <unicorn/unicorn.h>

namespace hl5 {

constexpr uint32_t NATIVE_CODE_BASE      = 0x40000000;
constexpr uint32_t NATIVE_HEAP_BASE      = 0x50000000;
constexpr uint32_t NATIVE_STACK_BASE     = 0x70000000;
constexpr uint32_t NATIVE_STACK_SIZE     = 0x00020000; // 128 KB
constexpr uint32_t NATIVE_TRAMPOLINE_BASE= 0x30000000;
constexpr uint32_t NATIVE_TRAMPOLINE_SIZE= 0x00010000; // 64 KB
constexpr uint32_t NATIVE_EXIT_MAGIC     = 0x0deadbee;

using HostHookFn = std::function<uint32_t(const std::vector<uint32_t>&)>;

class Arm32NativeEngine {
public:
    Arm32NativeEngine();
    ~Arm32NativeEngine();

    bool initialize();
    void close();

    // Memory mapping & allocation
    bool map_memory(uint32_t guest_addr, size_t size, uint32_t perms);
    bool write_memory(uint32_t guest_addr, const void* data, size_t size);
    bool read_memory(uint32_t guest_addr, void* out_data, size_t size);
    uint32_t allocate_heap(size_t size, size_t alignment = 8);
    uint32_t write_string(const std::string& str);

    // Host hooks & trampolines
    uint32_t register_host_hook(const std::string& name, HostHookFn fn);
    void setup_default_libc_hooks();

    // Function execution
    uint32_t call_function(uint32_t target_vaddr, const std::vector<uint32_t>& args, uint64_t timeout_us = 2000000);
    static void on_code_hook(uc_engine* uc, uint64_t address, uint32_t size, void* user_data);
    static void on_exit_hook(uc_engine* uc, uint64_t address, uint32_t size, void* user_data);

    uc_engine* uc_;
    uc_hook hook_code_;
    uc_hook hook_exit_;
    uint32_t heap_curr_;
    uint32_t next_trampoline_addr_;
    std::unordered_map<uint32_t, std::string> trampoline_map_;
    std::unordered_map<std::string, HostHookFn> host_hooks_;
};

} // namespace hl5

#endif // HL5_ARM32_NATIVE_ENGINE_H
