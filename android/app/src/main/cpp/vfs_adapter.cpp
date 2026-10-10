#include "vfs_adapter.h"
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <algorithm>

namespace hl5 {

VfsAdapter::VfsAdapter() : next_fd_(100), next_fp_(200) {}

VfsAdapter::~VfsAdapter() {
    close_all();
}

uint32_t VfsAdapter::register_asset(int host_fd, off_t start_offset, off_t length) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto stream = std::make_shared<VfsStream>();
    stream->host_fd = host_fd;
    stream->start_offset = start_offset;
    stream->length = length;
    stream->cursor = 0;
    stream->is_dup = false;
    stream->is_closed = false;

    uint32_t fd = next_fd_++;
    fd_table_[fd] = stream;
    return fd;
}

uint32_t VfsAdapter::hook_dup(uint32_t guest_fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = fd_table_.find(guest_fd);
    if (it == fd_table_.end() || it->second->is_closed) {
        return static_cast<uint32_t>(-1);
    }

    // Duplicate host OS file descriptor to allow independent positions
    int dup_host_fd = dup(it->second->host_fd);
    if (dup_host_fd < 0) {
        return static_cast<uint32_t>(-1);
    }

    auto cloned = std::make_shared<VfsStream>();
    cloned->host_fd = dup_host_fd;
    cloned->start_offset = it->second->start_offset;
    cloned->length = it->second->length;
    cloned->cursor = it->second->cursor;
    cloned->is_dup = true;
    cloned->is_closed = false;

    uint32_t new_fd = next_fd_++;
    fd_table_[new_fd] = cloned;
    return new_fd;
}

uint32_t VfsAdapter::hook_fdopen(uint32_t guest_fd, const char* /*mode*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = fd_table_.find(guest_fd);
    if (it == fd_table_.end() || it->second->is_closed) {
        return 0; // NULL
    }
    uint32_t fp_handle = next_fp_++;
    fp_table_[fp_handle] = it->second;
    return fp_handle;
}

int VfsAdapter::hook_fseek(uint32_t guest_fp, long offset, int whence) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = fp_table_.find(guest_fp);
    if (it == fp_table_.end() || it->second->is_closed) {
        return -1;
    }
    auto& stream = it->second;
    off_t target = 0;

    if (whence == SEEK_SET) {
        if (stream->start_offset > 0 && offset >= stream->start_offset) {
            target = offset - stream->start_offset;
        } else {
            target = offset;
        }
    } else if (whence == SEEK_CUR) {
        target = stream->cursor + offset;
    } else if (whence == SEEK_END) {
        target = stream->length + offset;
    } else {
        return -1;
    }

    if (target < 0) target = 0;
    if (target > stream->length) target = stream->length;

    stream->cursor = target;
    off_t real_pos = lseek(stream->host_fd, stream->start_offset + stream->cursor, SEEK_SET);
    return (real_pos >= 0) ? 0 : -1;
}

size_t VfsAdapter::hook_fread(uint32_t guest_ptr, size_t size, size_t nmemb, uint32_t guest_fp, GuestAddressSpace& addr_space) {
    size_t total_bytes = size * nmemb;
    if (total_bytes == 0) return 0;

    // 1. Validate Guest Address Space Range
    if (!addr_space.validate_guest_ptr(guest_ptr, total_bytes)) {
        return 0; // Out-of-bounds guest memory
    }
    void* host_dst = addr_space.to_host_ptr(guest_ptr);
    if (!host_dst) return 0;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = fp_table_.find(guest_fp);
    if (it == fp_table_.end() || it->second->is_closed) {
        return 0;
    }
    auto& stream = it->second;

    // 2. Bound to VFS window length
    off_t remaining = std::max(static_cast<off_t>(0), stream->length - stream->cursor);
    size_t to_read = std::min(total_bytes, static_cast<size_t>(remaining));
    if (to_read == 0) return 0;

    // Synchronize host file position before read
    lseek(stream->host_fd, stream->start_offset + stream->cursor, SEEK_SET);
    ssize_t bytes_read = read(stream->host_fd, host_dst, to_read);
    if (bytes_read > 0) {
        stream->cursor += bytes_read;
        return (size > 0) ? (static_cast<size_t>(bytes_read) / size) : 0;
    }
    return 0;
}

int VfsAdapter::hook_fclose(uint32_t guest_fp) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = fp_table_.find(guest_fp);
    if (it != fp_table_.end()) {
        auto stream = it->second;
        fp_table_.erase(it);

        if (stream->is_dup && !stream->is_closed) {
            close(stream->host_fd);
            stream->is_closed = true;
        }
        return 0;
    }
    return -1;
}

void VfsAdapter::close_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pair : fp_table_) {
        if (pair.second && pair.second->is_dup && !pair.second->is_closed) {
            close(pair.second->host_fd);
            pair.second->is_closed = true;
        }
    }
    fp_table_.clear();
    fd_table_.clear();
}

} // namespace hl5
