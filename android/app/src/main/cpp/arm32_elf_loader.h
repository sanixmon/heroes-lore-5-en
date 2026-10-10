#ifndef HL5_ARM32_ELF_LOADER_H
#define HL5_ARM32_ELF_LOADER_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <unordered_map>

namespace hl5 {

class Arm32NativeEngine;

struct SymbolInfo {
    std::string name;
    uint32_t vaddr;
    size_t size;
};

/**
 * Arm32ElfLoader
 * Maps ARM32 ELF shared libraries (libHeroesLore5.so) into Unicorn guest memory,
 * parses PT_LOAD segments, resolves dynamic symbols, and writes relocation entries
 * (.rel.plt and .rel.dyn) targeting host trampolines.
 */
class Arm32ElfLoader {
public:
    Arm32ElfLoader();
    ~Arm32ElfLoader();

    bool load_so(const std::string& filepath, Arm32NativeEngine& engine, uint32_t base_addr = 0x40000000);
    uint32_t find_symbol(const std::string& name) const;
    const std::unordered_map<std::string, SymbolInfo>& symbols() const { return symbols_; }

    uint32_t base_addr() const { return base_addr_; }
    size_t total_size() const { return total_size_; }

private:
    uint32_t base_addr_;
    size_t total_size_;
    std::unordered_map<std::string, SymbolInfo> symbols_;
};

} // namespace hl5

#endif // HL5_ARM32_ELF_LOADER_H
