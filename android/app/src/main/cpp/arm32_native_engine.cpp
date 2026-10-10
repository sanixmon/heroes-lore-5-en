#include "arm32_native_engine.h"
#include <unicorn/arm.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "HL5_Arm32Engine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do {} while(0)
#define LOGE(...) do {} while(0)
#endif
namespace hl5 {

Arm32NativeEngine::Arm32NativeEngine()
    : uc_(nullptr),
      hook_code_(0),
      hook_exit_(0),
      heap_curr_(NATIVE_HEAP_BASE),
      next_trampoline_addr_(NATIVE_TRAMPOLINE_BASE) {}

Arm32NativeEngine::~Arm32NativeEngine() {
    close();
}

void Arm32NativeEngine::close() {
    if (uc_) {
        uc_close(uc_);
        uc_ = nullptr;
    }
}

bool Arm32NativeEngine::initialize() {
    close();
    uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc_);
    if (err != UC_ERR_OK) {
        return false;
    }

    // 1. Map Stack: 0x70000000 .. 0x70020000
    if (!map_memory(NATIVE_STACK_BASE, NATIVE_STACK_SIZE, UC_PROT_READ | UC_PROT_WRITE)) {
        return false;
    }

    // 2. Map Heap: 0x50000000 .. 0x51000000 (16 MB)
    if (!map_memory(NATIVE_HEAP_BASE, 16 * 1024 * 1024, UC_PROT_READ | UC_PROT_WRITE)) {
        return false;
    }

    // 3. Map Trampoline Page: 0x30000000 .. 0x30010000 (64 KB)
    if (!map_memory(NATIVE_TRAMPOLINE_BASE, NATIVE_TRAMPOLINE_SIZE, UC_PROT_ALL)) {
        return false;
    }

    // Pre-fill trampoline area with 'bx lr' (0xe12fff1e)
    uint32_t bx_lr = 0xe12fff1e;
    std::vector<uint32_t> tramp_fill(NATIVE_TRAMPOLINE_SIZE / 4, bx_lr);
    write_memory(NATIVE_TRAMPOLINE_BASE, tramp_fill.data(), NATIVE_TRAMPOLINE_SIZE);

    // 4. Register code hook on trampoline range
    err = uc_hook_add(
        uc_,
        &hook_code_,
        UC_HOOK_CODE,
        reinterpret_cast<void*>(on_code_hook),
        this,
        NATIVE_TRAMPOLINE_BASE,
        NATIVE_TRAMPOLINE_BASE + NATIVE_TRAMPOLINE_SIZE
    );
    if (err != UC_ERR_OK) {
        return false;
    }
    // 5. Map exit magic page and hook so returning stops emulation cleanly
    uint32_t exit_page = NATIVE_EXIT_MAGIC & ~4095;
    if (!map_memory(exit_page, 4096, UC_PROT_ALL)) {
        return false;
    }
    uint32_t nop = 0xe1a00000; // mov r0, r0
    write_memory(NATIVE_EXIT_MAGIC, &nop, 4);
    err = uc_hook_add(
        uc_,
        &hook_exit_,
        UC_HOOK_CODE,
        reinterpret_cast<void*>(on_exit_hook),
        this,
        NATIVE_EXIT_MAGIC,
        NATIVE_EXIT_MAGIC + 4
    );
    if (err != UC_ERR_OK) {
        return false;
    }

    // 6. Setup standard libc hooks
    setup_default_libc_hooks();
    return true;
}

bool Arm32NativeEngine::map_memory(uint32_t guest_addr, size_t size, uint32_t perms) {
    size_t aligned_sz = (size + 4095) & ~4095;
    uc_err err = uc_mem_map(uc_, guest_addr, aligned_sz, perms);
    return (err == UC_ERR_OK);
}

bool Arm32NativeEngine::write_memory(uint32_t guest_addr, const void* data, size_t size) {
    uc_err err = uc_mem_write(uc_, guest_addr, data, size);
    return (err == UC_ERR_OK);
}

bool Arm32NativeEngine::read_memory(uint32_t guest_addr, void* out_data, size_t size) {
    uc_err err = uc_mem_read(uc_, guest_addr, out_data, size);
    return (err == UC_ERR_OK);
}

uint32_t Arm32NativeEngine::allocate_heap(size_t size, size_t alignment) {
    size_t aligned_sz = (size + alignment - 1) & ~(alignment - 1);
    uint32_t ptr = heap_curr_;
    heap_curr_ += aligned_sz;
    return ptr;
}

uint32_t Arm32NativeEngine::write_string(const std::string& str) {
    uint32_t ptr = allocate_heap(str.size() + 1, 8);
    write_memory(ptr, str.c_str(), str.size() + 1);
    return ptr;
}

uint32_t Arm32NativeEngine::register_host_hook(const std::string& name, HostHookFn fn) {
    for (const auto& pair : trampoline_map_) {
        if (pair.second == name) {
            host_hooks_[name] = std::move(fn);
            return pair.first;
        }
    }
    uint32_t t_addr = next_trampoline_addr_;
    next_trampoline_addr_ += 8;
    trampoline_map_[t_addr] = name;
    host_hooks_[name] = std::move(fn);
    return t_addr;
}

uint32_t Arm32NativeEngine::get_or_create_hook(const std::string& name) {
    for (const auto& pair : trampoline_map_) {
        if (pair.second == name) {
            return pair.first;
        }
    }
    return register_host_hook(name, [](const std::vector<uint32_t>&) -> uint32_t {
        return 0;
    });
}

void Arm32NativeEngine::setup_default_libc_hooks() {
    register_host_hook("memcpy", [this](const std::vector<uint32_t>& args) -> uint32_t {
        uint32_t dst = args[0], src = args[1], n = args[2];
        if (n > 0) {
            std::vector<uint8_t> buf(n);
            read_memory(src, buf.data(), n);
            write_memory(dst, buf.data(), n);
        }
        return dst;
    });

    register_host_hook("memset", [this](const std::vector<uint32_t>& args) -> uint32_t {
        uint32_t dst = args[0], val = args[1] & 0xFF, n = args[2];
        if (n > 0) {
            std::vector<uint8_t> buf(n, static_cast<uint8_t>(val));
            write_memory(dst, buf.data(), n);
        }
        return dst;
    });

    register_host_hook("strlen", [this](const std::vector<uint32_t>& args) -> uint32_t {
        uint32_t addr = args[0];
        uint32_t count = 0;
        char c = 0;
        while (read_memory(addr + count, &c, 1) && c != 0) {
            count++;
        }
        return count;
    });

    register_host_hook("malloc", [this](const std::vector<uint32_t>& args) -> uint32_t {
        return allocate_heap(args[0], 8);
    });

    register_host_hook("calloc", [this](const std::vector<uint32_t>& args) -> uint32_t {
        size_t total = args[0] * args[1];
        uint32_t ptr = allocate_heap(total, 8);
        std::vector<uint8_t> zero(total, 0);
        write_memory(ptr, zero.data(), total);
        return ptr;
    });

    register_host_hook("free", [](const std::vector<uint32_t>&) -> uint32_t {
        return 0;
    });
    register_host_hook("__android_log_print", [this](const std::vector<uint32_t>& args) -> uint32_t {
        if (args.size() >= 3) {
            int prio = static_cast<int>(args[0]);
            uint32_t tag_ptr = args[1];
            uint32_t fmt_ptr = args[2];
            char tag[64] = {0};
            char fmt[256] = {0};
            for (size_t i = 0; i < sizeof(tag) - 1; ++i) {
                if (!read_memory(tag_ptr + i, &tag[i], 1) || tag[i] == 0) break;
            }
            for (size_t i = 0; i < sizeof(fmt) - 1; ++i) {
                if (!read_memory(fmt_ptr + i, &fmt[i], 1) || fmt[i] == 0) break;
            }
#ifdef __ANDROID__
            __android_log_print(prio, tag[0] ? tag : "HL5_Guest", "%s", fmt);
#else
            std::cout << "[GUEST_LOG][" << (tag[0] ? tag : "HL5_Guest") << "] " << fmt << std::endl;
#endif
        }
        return 0;
    });
}

void Arm32NativeEngine::on_code_hook(uc_engine* uc, uint64_t address, uint32_t /*size*/, void* user_data) {
    auto* self = static_cast<Arm32NativeEngine*>(user_data);
    auto it = self->trampoline_map_.find(static_cast<uint32_t>(address));
    if (it == self->trampoline_map_.end()) {
        return;
    }

    uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, lr = 0, sp = 0;
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    uc_reg_read(uc, UC_ARM_REG_R1, &r1);
    uc_reg_read(uc, UC_ARM_REG_R2, &r2);
    uc_reg_read(uc, UC_ARM_REG_R3, &r3);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);

    std::vector<uint32_t> args = {r0, r1, r2, r3};
    if (sp >= NATIVE_STACK_BASE && sp <= (NATIVE_STACK_BASE + NATIVE_STACK_SIZE - 32)) {
        uint32_t stack_words[8] = {0};
        if (self->read_memory(sp, stack_words, sizeof(stack_words))) {
            for (int i = 0; i < 8; ++i) {
                args.push_back(stack_words[i]);
            }
        }
    }

    uint32_t ret_val = 0;
    auto hook_it = self->host_hooks_.find(it->second);
    if (hook_it != self->host_hooks_.end()) {
        ret_val = hook_it->second(args);
    }
    uc_reg_write(uc, UC_ARM_REG_R0, &ret_val);
    uc_reg_write(uc, UC_ARM_REG_PC, &lr);
}

void Arm32NativeEngine::on_exit_hook(uc_engine* uc, uint64_t /*address*/, uint32_t /*size*/, void* /*user_data*/) {
    uc_emu_stop(uc);
}

uint32_t Arm32NativeEngine::call_function(uint32_t target_vaddr, const std::vector<uint32_t>& args, uint64_t timeout_us) {
    // 1. Set CPSR to user mode (0x10) so banked SP/LR are active
    uint32_t cpsr = 0x10;
    uc_reg_write(uc_, UC_ARM_REG_CPSR, &cpsr);

    // 2. Set arguments in R0..R3
    int reg_ids[4] = {UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3};
    for (size_t i = 0; i < 4 && i < args.size(); ++i) {
        uint32_t val = args[i];
        uc_reg_write(uc_, reg_ids[i], &val);
    }

    // 3. Setup Stack and push args[4:] if len(args) > 4
    size_t extra_args = (args.size() > 4) ? (args.size() - 4) : 0;
    size_t stack_bytes = (extra_args * 4 + 7) & ~7;
    uint32_t sp = NATIVE_STACK_BASE + NATIVE_STACK_SIZE - 0x1000 - stack_bytes;

    for (size_t i = 0; i < extra_args; ++i) {
        uint32_t val = args[4 + i];
        write_memory(sp + (i * 4), &val, 4);
    }
    uc_reg_write(uc_, UC_ARM_REG_SP, &sp);

    // 4. Setup Link Register (LR) to return to NATIVE_EXIT_MAGIC
    uint32_t lr = NATIVE_EXIT_MAGIC;
    uc_reg_write(uc_, UC_ARM_REG_LR, &lr);

    // 5. Start emulation
    uc_err err = uc_emu_start(uc_, target_vaddr, 0, timeout_us, 0);
    if (err != UC_ERR_OK) {
        uint32_t pc = 0;
        uc_reg_read(uc_, UC_ARM_REG_PC, &pc);
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Execution failed at PC=0x%08x: %s", pc, uc_strerror(err));
        throw std::runtime_error(err_msg);
    }

    uint32_t ret_val = 0;
    uc_reg_read(uc_, UC_ARM_REG_R0, &ret_val);
    return ret_val;
}

} // namespace hl5
