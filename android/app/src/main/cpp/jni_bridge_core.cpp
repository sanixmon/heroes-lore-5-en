#include "jni_bridge_core.h"
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <cstdio>

namespace hl5 {

TypedHandleTable::TypedHandleTable() : next_id_(100) {
    local_frames_.emplace_back();
}

TypedHandleTable::~TypedHandleTable() {
    std::lock_guard<std::mutex> lock(mutex_);
    table_.clear();
}

uint32_t TypedHandleTable::allocate(HandleType type, void* host_ptr, const std::string& class_name, const std::string& sig) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t hid = next_id_++;
    HostObjectDescriptor desc;
    desc.handle_id = hid;
    desc.type = type;
    desc.host_ptr = host_ptr;
    desc.class_name = class_name;
    desc.signature = sig;
    desc.is_valid = true;

    table_[hid] = desc;
    if (type == HandleType::LOCAL_REF) {
        local_frames_.back().push_back(hid);
    }
    return hid;
}

void* TypedHandleTable::resolve(uint32_t handle_id, HandleType expected_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_.find(handle_id);
    if (it == table_.end()) {
        char err[128];
        snprintf(err, sizeof(err), "Invalid JNI handle: 0x%08x does not exist", handle_id);
        throw std::runtime_error(err);
    }
    if (!it->second.is_valid) {
        char err[128];
        snprintf(err, sizeof(err), "Use-after-free on JNI handle: 0x%08x", handle_id);
        throw std::runtime_error(err);
    }
    if (it->second.type != expected_type) {
        char err[128];
        snprintf(err, sizeof(err), "Type mismatch on JNI handle: 0x%08x", handle_id);
        throw std::runtime_error(err);
    }
    return it->second.host_ptr;
}

void TypedHandleTable::delete_ref(uint32_t handle_id, bool is_global) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_.find(handle_id);
    if (it != table_.end()) {
        HandleType expected = is_global ? HandleType::GLOBAL_REF : HandleType::LOCAL_REF;
        if (it->second.type == expected) {
            it->second.is_valid = false;
        }
    }
}

const HostObjectDescriptor* TypedHandleTable::find_descriptor(uint32_t handle_id) const {
    auto it = table_.find(handle_id);
    if (it == table_.end() || !it->second.is_valid) return nullptr;
    return &it->second;
}

void TypedHandleTable::push_local_frame() {
    std::lock_guard<std::mutex> lock(mutex_);
    local_frames_.emplace_back();
}

void TypedHandleTable::pop_local_frame() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (local_frames_.size() > 1) {
        auto& frame = local_frames_.back();
        for (uint32_t hid : frame) {
            auto it = table_.find(hid);
            if (it != table_.end() && it->second.type == HandleType::LOCAL_REF) {
                it->second.is_valid = false;
            }
        }
        local_frames_.pop_back();
    }
}

// -----------------------------------------------------------------------------
// GuestAddressSpace Implementation
// -----------------------------------------------------------------------------

GuestAddressSpace::GuestAddressSpace()
    : base_vaddr_(0x40000000), total_size_(0), current_offset_(0), host_mapping_(nullptr) {}

GuestAddressSpace::~GuestAddressSpace() {
    if (host_mapping_ && total_size_ > 0) {
        munmap(host_mapping_, total_size_);
    }
}

bool GuestAddressSpace::initialize(size_t guest_size) {
    total_size_ = (guest_size + 4095) & ~4095;
    // Map initial guest virtual address space.
    // On 64-bit Linux / Android, reserve address space without MAP_FIXED
    host_mapping_ = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(base_vaddr_)),
                         total_size_,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         -1, 0);

    if (host_mapping_ == MAP_FAILED) {
        // Fallback to anonymous mapping anywhere in address space
        host_mapping_ = mmap(nullptr, total_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (host_mapping_ == MAP_FAILED) {
            return false;
        }
    }
    return true;
}

uint32_t GuestAddressSpace::allocate_guest(size_t size, size_t alignment) {
    size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);
    if (current_offset_ + aligned_size > total_size_) {
        return 0; // Out of guest memory
    }
    uint32_t vaddr = base_vaddr_ + static_cast<uint32_t>(current_offset_);
    current_offset_ += aligned_size;
    return vaddr;
}

bool GuestAddressSpace::make_executable(uint32_t guest_vaddr, size_t size) {
    void* host_ptr = to_host_ptr(guest_vaddr);
    if (!host_ptr) return false;
    // Strict W^X compliance: remove PROT_WRITE when applying PROT_EXEC
    int ret = mprotect(host_ptr, size, PROT_READ | PROT_EXEC);
    if (ret == 0) {
        __builtin___clear_cache(reinterpret_cast<char*>(host_ptr),
                                reinterpret_cast<char*>(host_ptr) + size);
        return true;
    }
    return false;
}

bool GuestAddressSpace::make_writable(uint32_t guest_vaddr, size_t size) {
    void* host_ptr = to_host_ptr(guest_vaddr);
    if (!host_ptr) return false;
    // Remove PROT_EXEC when making writable
    return mprotect(host_ptr, size, PROT_READ | PROT_WRITE) == 0;
}

bool GuestAddressSpace::validate_guest_ptr(uint32_t guest_vaddr, size_t size) const {
    if (guest_vaddr < base_vaddr_) return false;
    uint32_t offset = guest_vaddr - base_vaddr_;
    return (offset + size) <= total_size_;
}

void* GuestAddressSpace::to_host_ptr(uint32_t guest_vaddr) const {
    if (!validate_guest_ptr(guest_vaddr, 1)) return nullptr;
    uint32_t offset = guest_vaddr - base_vaddr_;
    return static_cast<char*>(host_mapping_) + offset;
}

} // namespace hl5
