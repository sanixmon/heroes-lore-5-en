#ifndef HL5_VFS_ADAPTER_H
#define HL5_VFS_ADAPTER_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>
#include "jni_bridge_core.h"

namespace hl5 {

struct VfsStream {
    int host_fd;
    off_t start_offset;
    off_t length;
    off_t cursor;          // Relative to start_offset
    bool is_dup;
    bool is_closed;
};

/**
 * VfsAdapter
 * Cross-bitness virtual file system bridge for ARM32 guest on AArch64 host.
 *
 * Guarantees:
 *  1. Zero 64-bit pointer truncation: FILE* host objects are mapped to 32-bit handles.
 *  2. Guest Address Safety: All fread destinations are validated before write.
 *  3. VFS Window Boundary Enforcement: Seeks and reads are confined to [start_offset, start_offset + length].
 */
class VfsAdapter {
public:
    VfsAdapter();
    ~VfsAdapter();

    // Register an opened asset stream (e.g. from AAsset_openFileDescriptor)
    uint32_t register_asset(int host_fd, off_t start_offset, off_t length);

    // libc function trampolines called by ARM32 guest code
    uint32_t hook_dup(uint32_t guest_fd);
    uint32_t hook_fdopen(uint32_t guest_fd, const char* mode);
    int hook_fseek(uint32_t guest_fp, long offset, int whence);
    size_t hook_fread(uint32_t guest_ptr, size_t size, size_t nmemb, uint32_t guest_fp, GuestAddressSpace& addr_space);
    int hook_fclose(uint32_t guest_fp);

    void close_all();

private:
    std::mutex mutex_;
    uint32_t next_fd_;
    uint32_t next_fp_;
    std::unordered_map<uint32_t, std::shared_ptr<VfsStream>> fd_table_;
    std::unordered_map<uint32_t, std::shared_ptr<VfsStream>> fp_table_;
};

} // namespace hl5

#endif // HL5_VFS_ADAPTER_H
