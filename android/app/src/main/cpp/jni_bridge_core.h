#ifndef HL5_JNI_BRIDGE_CORE_H
#define HL5_JNI_BRIDGE_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace hl5 {

enum class HandleType {
    LOCAL_REF = 1,
    GLOBAL_REF = 2,
    METHOD_ID = 3,
    FIELD_ID = 4
};

struct HostObjectDescriptor {
    uint32_t handle_id;
    HandleType type;
    void* host_ptr;         // 64-bit host pointer (never truncated into 32-bit guest)
    std::string class_name;
    std::string signature;
    bool is_valid;
};

/**
 * TypedHandleTable
 * Stores 64-bit host pointers and maps them to 32-bit opaque handles (uint32_t).
 * Guarantees that host pointers are never truncated to 32 bits.
 */
class TypedHandleTable {
public:
    TypedHandleTable();
    ~TypedHandleTable();

    uint32_t allocate(HandleType type, void* host_ptr, const std::string& class_name = "", const std::string& sig = "");
    void* resolve(uint32_t handle_id, HandleType expected_type);
    void delete_ref(uint32_t handle_id, bool is_global);

    void push_local_frame();
    void pop_local_frame();

private:
    std::mutex mutex_;
    uint32_t next_id_;
    std::unordered_map<uint32_t, HostObjectDescriptor> table_;
    std::vector<std::vector<uint32_t>> local_frames_;
};

/**
 * GuestAddressSpace
 * Allocates and validates 32-bit guest virtual addresses in range < 4GB.
 * Implements strict SELinux W^X protection management (separate write vs execute phases).
 */
class GuestAddressSpace {
public:
    GuestAddressSpace();
    ~GuestAddressSpace();

    bool initialize(size_t guest_size);
    uint32_t allocate_guest(size_t size, size_t alignment = 4096);
    bool make_executable(uint32_t guest_vaddr, size_t size);
    bool make_writable(uint32_t guest_vaddr, size_t size);

    bool validate_guest_ptr(uint32_t guest_vaddr, size_t size) const;
    void* to_host_ptr(uint32_t guest_vaddr) const;

    uint32_t base_vaddr() const { return base_vaddr_; }
    size_t total_size() const { return total_size_; }

private:
    uint32_t base_vaddr_;
    size_t total_size_;
    size_t current_offset_;
    void* host_mapping_;
};

/**
 * 32-bit JNI Interface Structure Layout (Android ARM32 NDK ABI)
 */
struct JNINativeInterface32 {
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
    uint32_t GetVersion;                  // offset 0x10 (16)
    uint32_t DefineClass;
    uint32_t FindClass;                   // offset 0x18 (24)
    uint32_t FromReflectedMethod;
    uint32_t FromReflectedField;
    uint32_t ToReflectedMethod;
    uint32_t GetSuperclass;
    uint32_t IsAssignableFrom;
    uint32_t ToReflectedField;
    uint32_t Throw;
    uint32_t ThrowNew;
    uint32_t ExceptionOccurred;
    uint32_t ExceptionDescribe;
    uint32_t ExceptionClear;
    uint32_t FatalError;
    uint32_t PushLocalFrame;
    uint32_t PopLocalFrame;
    uint32_t NewGlobalRef;                // offset 0x54 (84)
    uint32_t DeleteGlobalRef;             // offset 0x58 (88)
    uint32_t DeleteLocalRef;              // offset 0x5c (92)
    uint32_t IsSameObject;
    uint32_t NewLocalRef;
    uint32_t EnsureLocalCapacity;
    // ... Additional standard JNI slots ...
    uint32_t table[220];                  // Remaining function pointers up to offset 0x36C (GetJavaVM)
};

} // namespace hl5

#endif // HL5_JNI_BRIDGE_CORE_H
