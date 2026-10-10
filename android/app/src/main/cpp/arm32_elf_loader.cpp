#include "arm32_elf_loader.h"
#include "arm32_native_engine.h"
#include <elf.h>
#include <fstream>
#include <cstring>
#include <iostream>

namespace hl5 {

Arm32ElfLoader::Arm32ElfLoader()
    : base_addr_(0x40000000), total_size_(0) {}

Arm32ElfLoader::~Arm32ElfLoader() {}

static inline uint32_t align_up(uint32_t val, uint32_t align) {
    return (val + align - 1) & ~(align - 1);
}

bool Arm32ElfLoader::load_so(const std::string& filepath, Arm32NativeEngine& engine, uint32_t base_addr) {
    base_addr_ = base_addr;
    symbols_.clear();

    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[ElfLoader] Cannot open file: " << filepath << std::endl;
        return false;
    }

    // 1. Read ELF Header
    Elf32_Ehdr ehdr;
    file.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr));
    if (file.gcount() != sizeof(ehdr)) return false;

    if (ehdr.e_ident[EI_MAG0] != ELFMAG0 || ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr.e_ident[EI_MAG2] != ELFMAG2 || ehdr.e_ident[EI_MAG3] != ELFMAG3) {
        std::cerr << "[ElfLoader] Invalid ELF magic" << std::endl;
        return false;
    }
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS32 || ehdr.e_machine != EM_ARM) {
        std::cerr << "[ElfLoader] Not 32-bit ARM ELF" << std::endl;
        return false;
    }

    // 2. Read Program Headers
    std::vector<Elf32_Phdr> phdrs(ehdr.e_phnum);
    file.seekg(ehdr.e_phoff, std::ios::beg);
    file.read(reinterpret_cast<char*>(phdrs.data()), ehdr.e_phnum * sizeof(Elf32_Phdr));

    uint32_t min_vaddr = 0xFFFFFFFF;
    uint32_t max_vaddr = 0;
    for (const auto& ph : phdrs) {
        if (ph.p_type == PT_LOAD) {
            if (ph.p_vaddr < min_vaddr) min_vaddr = ph.p_vaddr;
            uint32_t seg_end = ph.p_vaddr + ph.p_memsz;
            if (seg_end > max_vaddr) max_vaddr = seg_end;
        }
    }

    total_size_ = align_up(max_vaddr - min_vaddr, 4096);
    if (!engine.map_memory(base_addr_, total_size_, UC_PROT_ALL)) {
        std::cerr << "[ElfLoader] Failed to map memory for ELF segments" << std::endl;
        return false;
    }

    // 3. Load PT_LOAD Segments into Guest Memory
    for (const auto& ph : phdrs) {
        if (ph.p_type == PT_LOAD) {
            std::vector<char> seg_data(ph.p_memsz, 0);
            if (ph.p_filesz > 0) {
                file.seekg(ph.p_offset, std::ios::beg);
                file.read(seg_data.data(), ph.p_filesz);
            }
            engine.write_memory(base_addr_ + ph.p_vaddr, seg_data.data(), ph.p_memsz);
        }
    }

    // 4. Read Section Headers & String Tables
    std::vector<Elf32_Shdr> shdrs(ehdr.e_shnum);
    file.seekg(ehdr.e_shoff, std::ios::beg);
    file.read(reinterpret_cast<char*>(shdrs.data()), ehdr.e_shnum * sizeof(Elf32_Shdr));

    // Section header string table
    std::vector<char> shstrtab;
    if (ehdr.e_shstrndx < ehdr.e_shnum) {
        shstrtab.resize(shdrs[ehdr.e_shstrndx].sh_size);
        file.seekg(shdrs[ehdr.e_shstrndx].sh_offset, std::ios::beg);
        file.read(shstrtab.data(), shdrs[ehdr.e_shstrndx].sh_size);
    }

    std::vector<Elf32_Sym> dynsyms;
    std::vector<char> dynstr;
    std::vector<Elf32_Rel> rel_plt;
    std::vector<Elf32_Rel> rel_dyn;

    for (const auto& sh : shdrs) {
        const char* sname = shstrtab.empty() ? "" : &shstrtab[sh.sh_name];
        if (sh.sh_type == SHT_DYNSYM || strcmp(sname, ".dynsym") == 0) {
            dynsyms.resize(sh.sh_size / sizeof(Elf32_Sym));
            file.seekg(sh.sh_offset, std::ios::beg);
            file.read(reinterpret_cast<char*>(dynsyms.data()), sh.sh_size);
        } else if (sh.sh_type == SHT_STRTAB && strcmp(sname, ".dynstr") == 0) {
            dynstr.resize(sh.sh_size);
            file.seekg(sh.sh_offset, std::ios::beg);
            file.read(dynstr.data(), sh.sh_size);
        } else if (sh.sh_type == SHT_REL && strcmp(sname, ".rel.plt") == 0) {
            rel_plt.resize(sh.sh_size / sizeof(Elf32_Rel));
            file.seekg(sh.sh_offset, std::ios::beg);
            file.read(reinterpret_cast<char*>(rel_plt.data()), sh.sh_size);
        } else if (sh.sh_type == SHT_REL && strcmp(sname, ".rel.dyn") == 0) {
            rel_dyn.resize(sh.sh_size / sizeof(Elf32_Rel));
            file.seekg(sh.sh_offset, std::ios::beg);
            file.read(reinterpret_cast<char*>(rel_dyn.data()), sh.sh_size);
        }
    }

    // 5. Parse Dynamic Symbols
    for (const auto& sym : dynsyms) {
        if (sym.st_name < dynstr.size()) {
            const char* name = &dynstr[sym.st_name];
            if (name && name[0] != '\0') {
                symbols_[name] = SymbolInfo{name, base_addr_ + sym.st_value, sym.st_size};
            }
        }
    }

    // 6. Apply .rel.plt Relocations (R_ARM_JUMP_SLOT)
    for (const auto& rel : rel_plt) {
        uint32_t type = ELF32_R_TYPE(rel.r_info);
        uint32_t sym_idx = ELF32_R_SYM(rel.r_info);
        if (type == R_ARM_JUMP_SLOT && sym_idx < dynsyms.size()) {
            const char* sym_name = &dynstr[dynsyms[sym_idx].st_name];
            uint32_t tramp_addr = engine.get_or_create_hook(sym_name);
            engine.write_memory(base_addr_ + rel.r_offset, &tramp_addr, sizeof(tramp_addr));
        }
    }

    // 7. Apply .rel.dyn Relocations (R_ARM_RELATIVE, R_ARM_GLOB_DAT)
    for (const auto& rel : rel_dyn) {
        uint32_t type = ELF32_R_TYPE(rel.r_info);
        uint32_t sym_idx = ELF32_R_SYM(rel.r_info);

        if (type == R_ARM_RELATIVE) {
            uint32_t val = 0;
            engine.read_memory(base_addr_ + rel.r_offset, &val, sizeof(val));
            val += base_addr_;
            engine.write_memory(base_addr_ + rel.r_offset, &val, sizeof(val));
        } else if (type == R_ARM_GLOB_DAT && sym_idx < dynsyms.size()) {
            const char* sym_name = &dynstr[dynsyms[sym_idx].st_name];
            if (strcmp(sym_name, "__stack_chk_guard") == 0) {
                uint32_t canary = 0x50000000;
                engine.write_memory(base_addr_ + rel.r_offset, &canary, sizeof(canary));
            } else {
                auto it = symbols_.find(sym_name);
                uint32_t target_addr = 0;
                if (it != symbols_.end() && it->second.vaddr != base_addr_) {
                    target_addr = it->second.vaddr;
                } else {
                    target_addr = engine.get_or_create_hook(sym_name);
                }
                engine.write_memory(base_addr_ + rel.r_offset, &target_addr, sizeof(target_addr));
            }
        }
    }

    return true;
}

uint32_t Arm32ElfLoader::find_symbol(const std::string& name) const {
    auto it = symbols_.find(name);
    if (it != symbols_.end()) {
        return it->second.vaddr;
    }
    return 0;
}

} // namespace hl5
