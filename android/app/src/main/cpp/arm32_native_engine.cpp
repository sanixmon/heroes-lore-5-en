#include "arm32_native_engine.h"
#include <unicorn/arm.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cerrno>
#include "hl5_save_trace.h"
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

std::string Arm32NativeEngine::read_string(uint32_t guest_addr, size_t max_len) {
    if (guest_addr == 0) return "";
    std::string result;
    result.reserve(64);
    for (size_t i = 0; i < max_len; ++i) {
        char c = 0;
        if (!read_memory(guest_addr + i, &c, 1) || c == '\0') break;
        result.push_back(c);
    }
    return result;
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

// Save-path fd tracker for targeted diagnostics: maps host fds opened for
// LOCAL.sav / kickass / SL_*.sav so read/write/lseek/close logs can show the
// path. Guest emulation is single-threaded; no locking (matches existing hooks).
static std::unordered_map<int, std::string> g_hl5_save_fd_paths;

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

    register_host_hook("strcpy", [this](const std::vector<uint32_t>& args) -> uint32_t {
        uint32_t dst = args[0], src = args[1];
        std::string s = read_string(src);
        write_memory(dst, s.c_str(), s.size() + 1);
        return dst;
    });

    register_host_hook("strncpy", [this](const std::vector<uint32_t>& args) -> uint32_t {
        uint32_t dst = args[0], src = args[1], n = args[2];
        std::string s = read_string(src, n);
        std::vector<uint8_t> buf(n, 0);
        memcpy(buf.data(), s.data(), std::min(s.size(), static_cast<size_t>(n)));
        write_memory(dst, buf.data(), n);
        return dst;
    });

    register_host_hook("strcat", [this](const std::vector<uint32_t>& args) -> uint32_t {
        uint32_t dst = args[0], src = args[1];
        std::string s1 = read_string(dst);
        std::string s2 = read_string(src);
        std::string merged = s1 + s2;
        write_memory(dst, merged.c_str(), merged.size() + 1);
        return dst;
    });

    register_host_hook("strcmp", [this](const std::vector<uint32_t>& args) -> uint32_t {
        std::string s1 = read_string(args[0]);
        std::string s2 = read_string(args[1]);
        if (s1 == s2) return 0;
        return (s1 > s2) ? 1 : static_cast<uint32_t>(-1);
    });

    register_host_hook("strncmp", [this](const std::vector<uint32_t>& args) -> uint32_t {
        size_t n = args[2];
        std::string s1 = read_string(args[0], n);
        std::string s2 = read_string(args[1], n);
        if (s1 == s2) return 0;
        return (s1 > s2) ? 1 : static_cast<uint32_t>(-1);
    });

    register_host_hook("connect", [](const std::vector<uint32_t>&) -> uint32_t {
        return 0xFFFFFFFF; // Offline mock
    });

    register_host_hook("socket", [](const std::vector<uint32_t>&) -> uint32_t {
        return 0xFFFFFFFF; // Offline mock
    });

    register_host_hook("pow", [this](const std::vector<uint32_t>& args) -> uint32_t {
        uint64_t x_bits = (static_cast<uint64_t>(args[1]) << 32) | args[0];
        uint64_t y_bits = (static_cast<uint64_t>(args[3]) << 32) | args[2];
        double x = 0, y = 0;
        memcpy(&x, &x_bits, sizeof(double));
        memcpy(&y, &y_bits, sizeof(double));
        double res = std::pow(x, y);
        uint64_t res_bits = 0;
        memcpy(&res_bits, &res, sizeof(double));
        // ARM32 EABI: double return in R0 (low) + R1 (high)
        uint32_t r1_val = static_cast<uint32_t>((res_bits >> 32) & 0xFFFFFFFF);
        uc_reg_write(uc_, UC_ARM_REG_R1, &r1_val);
        return static_cast<uint32_t>(res_bits & 0xFFFFFFFF);
    });

    register_host_hook("ftime", [this](const std::vector<uint32_t>& args) -> uint32_t {
        uint32_t tp_ptr = args[0];
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        uint32_t sec = static_cast<uint32_t>(tv.tv_sec);
        uint16_t millitm = static_cast<uint16_t>(tv.tv_usec / 1000);
        int16_t timezone = 0;
        int16_t dstflag = 0;
        write_memory(tp_ptr, &sec, 4);
        write_memory(tp_ptr + 4, &millitm, 2);
        write_memory(tp_ptr + 6, &timezone, 2);
        write_memory(tp_ptr + 8, &dstflag, 2);
        return 0;
    });

    register_host_hook("vsnprintf", [this](const std::vector<uint32_t>& args) -> uint32_t {
        uint32_t buf_ptr = args[0];
        uint32_t size = args[1];
        uint32_t fmt_ptr = args[2];
        uint32_t va_ptr = args[3];
        std::string fmt = read_string(fmt_ptr);

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
                size_t j = i + 1;
                while (j < fmt.size() && (isdigit(fmt[j]) || fmt[j] == '-' || fmt[j] == '+' || fmt[j] == ' ' || fmt[j] == '#' || fmt[j] == '.' || fmt[j] == 'l' || fmt[j] == 'h')) {
                    j++;
                }
                if (j < fmt.size()) {
                    char conv = fmt[j];
                    std::string spec = fmt.substr(i, j - i + 1);
                    uint32_t val = 0;
                    read_memory(cur_va, &val, 4);
                    cur_va += 4;

                    char formatted[128] = {0};
                    if (conv == 's') {
                        std::string s = read_string(val);
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
            write_memory(buf_ptr, out.c_str(), copy_len);
            char null_byte = 0;
            write_memory(buf_ptr + copy_len, &null_byte, 1);
        }
        return static_cast<uint32_t>(out.size());
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

    register_host_hook("strdup", [this](const std::vector<uint32_t>& args) -> uint32_t {
        std::string s = read_string(args[0]);
        uint32_t ptr = allocate_heap(s.size() + 1, 4);
        write_memory(ptr, s.c_str(), s.size() + 1);
        return ptr;
    });

    register_host_hook("__errno", [this](const std::vector<uint32_t>&) -> uint32_t {
        // Return a stable guest address for errno (4 bytes, always 0)
        static uint32_t errno_addr = 0;
        if (errno_addr == 0) {
            errno_addr = allocate_heap(4, 4);
            uint32_t zero = 0;
            write_memory(errno_addr, &zero, 4);
        }
        return errno_addr;
    });

    register_host_hook("memmove", [this](const std::vector<uint32_t>& args) -> uint32_t {
        uint32_t dst = args[0], src = args[1], n = args[2];
        if (n > 0) {
            std::vector<uint8_t> buf(n);
            read_memory(src, buf.data(), n);
            write_memory(dst, buf.data(), n);
        }
        return dst;
    });

    // POSIX Filesystem Hooks for Save/Load Persistence (MC_fsOpen, MC_fsRead, MC_fsWrite, etc.)
    register_host_hook("stat", [this](const std::vector<uint32_t>& args) -> uint32_t {
        std::string path = read_string(args[0]);
        uint32_t stat_buf_ptr = args[1];
        if (path.empty()) return 0xFFFFFFFF;
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) {
            int e = errno;
            if (hl5_is_save_path(path)) {
                HL5_SAVE_LOG("stat path=\"%s\" -> FAIL errno=%d(%s)", path.c_str(), e, strerror(e));
            }
            return 0xFFFFFFFF;
        }
        if (stat_buf_ptr >= 0x10000) {
            uint8_t guest_stat[104] = {0};
            uint32_t st_size_32 = static_cast<uint32_t>(st.st_size);
            uint32_t st_mtime_32 = static_cast<uint32_t>(st.st_mtime);
            std::memcpy(guest_stat + 48, &st_size_32, 4);
            std::memcpy(guest_stat + 80, &st_mtime_32, 4);
            write_memory(stat_buf_ptr, guest_stat, sizeof(guest_stat));
        }
        if (hl5_is_save_path(path)) {
            HL5_SAVE_LOG("stat path=\"%s\" -> ok size=%lld mtime=%lld", path.c_str(),
                         (long long)st.st_size, (long long)st.st_mtime);
        }
        return 0;
    });

    register_host_hook("open", [this](const std::vector<uint32_t>& args) -> uint32_t {
        std::string path = read_string(args[0]);
        int flags = static_cast<int>(args[1]);
        mode_t mode = (args.size() > 2 && args[2] != 0) ? static_cast<mode_t>(args[2]) : 0666;
        if (path.empty()) return 0xFFFFFFFF;
        int fd = ::open(path.c_str(), flags, mode);
        LOGI("[FS] open(\"%s\", 0x%x, 0%o) -> fd=%d", path.c_str(), flags, mode, fd);
        if (hl5_is_save_path(path)) {
            if (fd >= 0) {
                g_hl5_save_fd_paths[fd] = path;
                struct stat st;
                long long sz = -1;
                if (::fstat(fd, &st) == 0) sz = (long long)st.st_size;
                HL5_SAVE_LOG("open path=\"%s\" flags=0x%x mode=0%o -> fd=%d size=%lld",
                             path.c_str(), flags, mode, fd, sz);
            } else {
                int e = errno;
                HL5_SAVE_LOG("open path=\"%s\" flags=0x%x mode=0%o -> FAIL errno=%d(%s)",
                             path.c_str(), flags, mode, e, strerror(e));
            }
        }
        return (fd >= 0) ? static_cast<uint32_t>(fd) : 0xFFFFFFFF;
    });

    register_host_hook("read", [this](const std::vector<uint32_t>& args) -> uint32_t {
        int fd = static_cast<int>(args[0]);
        uint32_t buf_ptr = args[1];
        size_t count = static_cast<size_t>(args[2]);
        if (fd < 0 || count == 0) return (fd < 0) ? 0xFFFFFFFF : 0;
        std::vector<uint8_t> tmp(count);
        ssize_t n = ::read(fd, tmp.data(), count);
        if (n > 0) {
            write_memory(buf_ptr, tmp.data(), static_cast<size_t>(n));
        }
        auto it = g_hl5_save_fd_paths.find(fd);
        if (it != g_hl5_save_fd_paths.end()) {
            if (n >= 0) {
                HL5_SAVE_LOG("read path=\"%s\" fd=%d req=%u got=%d", it->second.c_str(), fd,
                             (unsigned)count, (int)n);
            } else {
                int e = errno;
                HL5_SAVE_LOG("read path=\"%s\" fd=%d req=%u -> FAIL errno=%d(%s)",
                             it->second.c_str(), fd, (unsigned)count, e, strerror(e));
            }
        }
        return (n >= 0) ? static_cast<uint32_t>(n) : 0xFFFFFFFF;
    });

    register_host_hook("write", [this](const std::vector<uint32_t>& args) -> uint32_t {
        int fd = static_cast<int>(args[0]);
        uint32_t buf_ptr = args[1];
        size_t count = static_cast<size_t>(args[2]);
        if (fd < 0 || count == 0) return (fd < 0) ? 0xFFFFFFFF : 0;
        std::vector<uint8_t> tmp(count);
        if (!read_memory(buf_ptr, tmp.data(), count)) {
            return 0xFFFFFFFF;
        }
        ssize_t n = ::write(fd, tmp.data(), count);
        if (n > 0) {
            ::fsync(fd);
        }
        auto it = g_hl5_save_fd_paths.find(fd);
        if (it != g_hl5_save_fd_paths.end()) {
            if (n >= 0) {
                HL5_SAVE_LOG("write path=\"%s\" fd=%d req=%u wrote=%d(fsynced)", it->second.c_str(),
                             fd, (unsigned)count, (int)n);
            } else {
                int e = errno;
                HL5_SAVE_LOG("write path=\"%s\" fd=%d req=%u -> FAIL errno=%d(%s)",
                             it->second.c_str(), fd, (unsigned)count, e, strerror(e));
            }
        }
        return (n >= 0) ? static_cast<uint32_t>(n) : 0xFFFFFFFF;
    });

    register_host_hook("lseek", [](const std::vector<uint32_t>& args) -> uint32_t {
        int fd = static_cast<int>(args[0]);
        off_t offset = static_cast<int32_t>(args[1]);
        int whence = static_cast<int>(args[2]);
        off_t res = ::lseek(fd, offset, whence);
        auto it = g_hl5_save_fd_paths.find(fd);
        if (it != g_hl5_save_fd_paths.end()) {
            if (res >= 0) {
                HL5_SAVE_LOG("lseek path=\"%s\" fd=%d off=%d whence=%d -> pos=%d",
                             it->second.c_str(), fd, (int)offset, whence, (int)res);
            } else {
                int e = errno;
                HL5_SAVE_LOG("lseek path=\"%s\" fd=%d off=%d whence=%d -> FAIL errno=%d(%s)",
                             it->second.c_str(), fd, (int)offset, whence, e, strerror(e));
            }
        }
        return (res >= 0) ? static_cast<uint32_t>(res) : 0xFFFFFFFF;
    });

    register_host_hook("close", [](const std::vector<uint32_t>& args) -> uint32_t {
        int fd = static_cast<int>(args[0]);
        if (fd > 2) {
            auto it = g_hl5_save_fd_paths.find(fd);
            uint32_t rc = static_cast<uint32_t>(::close(fd));
            if (it != g_hl5_save_fd_paths.end()) {
                if (rc == 0) {
                    HL5_SAVE_LOG("close path=\"%s\" fd=%d -> ok", it->second.c_str(), fd);
                } else {
                    int e = errno;
                    HL5_SAVE_LOG("close path=\"%s\" fd=%d -> FAIL errno=%d(%s)",
                                 it->second.c_str(), fd, e, strerror(e));
                }
                g_hl5_save_fd_paths.erase(it);
            }
            return rc;
        }
        return 0;
    });

    register_host_hook("unlink", [this](const std::vector<uint32_t>& args) -> uint32_t {
        std::string path = read_string(args[0]);
        if (path.empty()) return 0xFFFFFFFF;
        uint32_t rc = static_cast<uint32_t>(::unlink(path.c_str()));
        if (hl5_is_save_path(path)) {
            if (rc == 0) {
                HL5_SAVE_LOG("remove/unlink path=\"%s\" -> ok (note: may be LoadLocal gv+0x29 branch; behavior unchanged)",
                             path.c_str());
            } else {
                int e = errno;
                HL5_SAVE_LOG("remove/unlink path=\"%s\" -> FAIL errno=%d(%s)", path.c_str(), e,
                             strerror(e));
            }
        }
        return rc;
    });

    register_host_hook("rename", [this](const std::vector<uint32_t>& args) -> uint32_t {
        std::string old_p = read_string(args[0]);
        std::string new_p = read_string(args[1]);
        if (old_p.empty() || new_p.empty()) return 0xFFFFFFFF;
        uint32_t rc = static_cast<uint32_t>(::rename(old_p.c_str(), new_p.c_str()));
        if (hl5_is_save_path(old_p) || hl5_is_save_path(new_p)) {
            if (rc == 0) {
                HL5_SAVE_LOG("rename old=\"%s\" new=\"%s\" -> ok", old_p.c_str(), new_p.c_str());
            } else {
                int e = errno;
                HL5_SAVE_LOG("rename old=\"%s\" new=\"%s\" -> FAIL errno=%d(%s)", old_p.c_str(),
                             new_p.c_str(), e, strerror(e));
            }
        }
        return rc;
    });

    register_host_hook("access", [this](const std::vector<uint32_t>& args) -> uint32_t {
        std::string path = read_string(args[0]);
        int mode = static_cast<int>(args[1]);
        if (path.empty()) return 0xFFFFFFFF;
        uint32_t rc = static_cast<uint32_t>(::access(path.c_str(), mode));
        if (hl5_is_save_path(path)) {
            HL5_SAVE_LOG("access path=\"%s\" mode=%d -> %d%s", path.c_str(), mode, (int)rc,
                         (rc != 0 ? strerror(errno) : "ok"));
        }
        return rc;
    });

    register_host_hook("mkdir", [this](const std::vector<uint32_t>& args) -> uint32_t {
        std::string path = read_string(args[0]);
        mode_t mode = (args.size() > 1 && args[1] != 0) ? static_cast<mode_t>(args[1]) : 0777;
        if (path.empty()) return 0xFFFFFFFF;
        return static_cast<uint32_t>(::mkdir(path.c_str(), mode));
    });

    register_host_hook("rmdir", [this](const std::vector<uint32_t>& args) -> uint32_t {
        std::string path = read_string(args[0]);
        if (path.empty()) return 0xFFFFFFFF;
        return static_cast<uint32_t>(::rmdir(path.c_str()));
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
