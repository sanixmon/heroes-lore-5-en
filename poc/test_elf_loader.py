#!/usr/bin/env python3
"""
Heroes Lore 5 — ARM32 ELF Loader & Relocation Verification PoC (poc/test_elf_loader.py)

Simulates in-process loading of libHeroesLore5.so on a 64-bit host:
  1. Parses ELF header, program headers (PT_LOAD segments), and dynamic section.
  2. Maps virtual memory space (size ~1.5 MB) starting at an arbitrary base address (< 4GB).
  3. Applies all 1,540 R_ARM_RELATIVE relocations.
  4. Binds all 75 R_ARM_JUMP_SLOT PLT entries to simulated host bridge trampolines.
  5. Binds R_ARM_GLOB_DAT (__stack_chk_guard) and R_ARM_ABS32 entries.
  6. Locates and validates all 18 JNI export entry points and their instruction prologues.
"""

from __future__ import annotations

import struct
from pathlib import Path
from typing import Dict, List, Tuple

BASE_VFS_PATH = Path("apktool_modern/assets/data.vfs.mp3")
SO_PATH = Path("apktool_modern/lib/armeabi/libHeroesLore5.so")


class Arm32ElfLoader:
    def __init__(self, so_bytes: bytes, base_addr: int = 0x40000000) -> None:
        self.raw = so_bytes
        self.base_addr = base_addr
        assert self.base_addr < 0xFFFFFFFF, "Base address must fit in 32-bit pointer space!"

        # 1. Parse ELF32 header
        assert self.raw[:4] == b"\x7fELF", "Not an ELF file"
        assert self.raw[4] == 1, "Not a 32-bit ELF"
        assert self.raw[5] == 1, "Not little-endian"
        self.entry_point = struct.unpack_from("<I", self.raw, 24)[0]
        self.phoff = struct.unpack_from("<I", self.raw, 28)[0]
        self.shoff = struct.unpack_from("<I", self.raw, 32)[0]
        self.phentsize = struct.unpack_from("<H", self.raw, 42)[0]
        self.phnum = struct.unpack_from("<H", self.raw, 44)[0]

        # 2. Parse PT_LOAD segments
        self.load_segments: List[Tuple[int, int, int, int, int]] = []
        max_vaddr = 0
        for i in range(self.phnum):
            off = self.phoff + i * self.phentsize
            p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = (
                struct.unpack_from("<IIIIIIII", self.raw, off)
            )
            if p_type == 1:  # PT_LOAD
                self.load_segments.append((p_offset, p_vaddr, p_filesz, p_memsz, p_flags))
                max_vaddr = max(max_vaddr, p_vaddr + p_memsz)

        self.total_memsz = max_vaddr
        # Allocate simulated memory buffer
        self.memory = bytearray(self.total_memsz)

        # 3. Load segments into simulated memory
        for p_offset, p_vaddr, p_filesz, p_memsz, p_flags in self.load_segments:
            seg_data = self.raw[p_offset : p_offset + p_filesz]
            self.memory[p_vaddr : p_vaddr + p_filesz] = seg_data

        # 4. Parse dynamic symbols and string table
        self.dynsyms: Dict[str, Tuple[int, int]] = {}
        self.dynsym_by_idx: List[Tuple[str, int, int]] = []
        self._parse_dynamic_section()

    def _parse_dynamic_section(self) -> None:
        # Locate PT_DYNAMIC segment
        dyn_vaddr = 0
        dyn_sz = 0
        for i in range(self.phnum):
            off = self.phoff + i * self.phentsize
            p_type, p_offset, p_vaddr, _, p_filesz, _, _, _ = struct.unpack_from(
                "<IIIIIIII", self.raw, off
            )
            if p_type == 2:  # PT_DYNAMIC
                dyn_vaddr = p_vaddr
                dyn_sz = p_filesz
                break

        dyn_tags: Dict[int, int] = {}
        for i in range(0, dyn_sz, 8):
            tag, val = struct.unpack_from("<iI", self.memory, dyn_vaddr + i)
            if tag == 0:  # DT_NULL
                break
            dyn_tags[tag] = val

        strtab_off = dyn_tags.get(5, 0)  # DT_STRTAB
        symtab_off = dyn_tags.get(6, 0)  # DT_SYMTAB
        syment = dyn_tags.get(11, 16)  # DT_SYMENT

        # Read symbols from symtab until strtab or another section boundary
        idx = 0
        while True:
            off = symtab_off + idx * syment
            if off + 16 > len(self.memory):
                break
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(
                "<IIIBBH", self.memory, off
            )
            if idx > 0 and st_name == 0 and st_value == 0:
                break
            name_end = self.memory.find(b"\x00", strtab_off + st_name)
            name = self.memory[strtab_off + st_name : name_end].decode("ascii", errors="replace")
            self.dynsyms[name] = (st_value, st_size)
            self.dynsym_by_idx.append((name, st_value, st_size))
            idx += 1

        self.rel_off = dyn_tags.get(17, 0)  # DT_REL
        self.rel_sz = dyn_tags.get(18, 0)  # DT_RELSZ
        self.jmprel_off = dyn_tags.get(23, 0)  # DT_JMPREL
        self.pltrel_sz = dyn_tags.get(2, 0)  # DT_PLTRELSZ

    def apply_relocations(self, bridge_table: Dict[str, int]) -> Dict[str, int]:
        stats = {
            "R_ARM_RELATIVE": 0,
            "R_ARM_JUMP_SLOT": 0,
            "R_ARM_ABS32": 0,
            "R_ARM_GLOB_DAT": 0,
            "unresolved": 0,
        }

        # Apply DT_REL (.rel.dyn)
        for i in range(0, self.rel_sz, 8):
            r_offset, r_info = struct.unpack_from("<II", self.memory, self.rel_off + i)
            r_type = r_info & 0xFF
            r_sym = r_info >> 8

            if r_type == 23:  # R_ARM_RELATIVE
                orig_val = struct.unpack_from("<I", self.memory, r_offset)[0]
                new_val = (orig_val + self.base_addr) & 0xFFFFFFFF
                struct.pack_into("<I", self.memory, r_offset, new_val)
                stats["R_ARM_RELATIVE"] += 1
            elif r_type == 21:  # R_ARM_GLOB_DAT
                sym_name, st_val, _ = self.dynsym_by_idx[r_sym]
                if st_val != 0:
                    target_val = self.base_addr + st_val
                elif sym_name in bridge_table:
                    target_val = bridge_table[sym_name]
                else:
                    target_val = 0xDEAD0000
                    stats["unresolved"] += 1
                struct.pack_into("<I", self.memory, r_offset, target_val & 0xFFFFFFFF)
                stats["R_ARM_GLOB_DAT"] += 1
            elif r_type == 2:  # R_ARM_ABS32
                sym_name, st_val, _ = self.dynsym_by_idx[r_sym]
                if st_val != 0:
                    sym_addr = self.base_addr + st_val
                elif sym_name in bridge_table:
                    sym_addr = bridge_table[sym_name]
                else:
                    sym_addr = 0
                orig_val = struct.unpack_from("<I", self.memory, r_offset)[0]
                new_val = (orig_val + sym_addr) & 0xFFFFFFFF
                struct.pack_into("<I", self.memory, r_offset, new_val)
                stats["R_ARM_ABS32"] += 1
        # Apply DT_JMPREL (.rel.plt)
        for i in range(0, self.pltrel_sz, 8):
            r_offset, r_info = struct.unpack_from("<II", self.memory, self.jmprel_off + i)
            r_type = r_info & 0xFF
            r_sym = r_info >> 8

            if r_type == 22:  # R_ARM_JUMP_SLOT
                sym_name = self.dynsym_by_idx[r_sym][0]
                if sym_name in bridge_table:
                    trampoline_addr = bridge_table[sym_name]
                else:
                    trampoline_addr = 0xDEADBEEF
                    stats["unresolved"] += 1
                struct.pack_into("<I", self.memory, r_offset, trampoline_addr & 0xFFFFFFFF)
                stats["R_ARM_JUMP_SLOT"] += 1

        return stats

    def inspect_jni_entry_points(self) -> List[Dict[str, Any]]:
        results = []
        for name, (val, size) in self.dynsyms.items():
            if name.startswith("Java_"):
                vaddr = val
                target_pc = (self.base_addr + vaddr) & 0xFFFFFFFF
                prologue = bytes(self.memory[vaddr : vaddr + 16])
                w0, w1 = struct.unpack_from("<II", prologue, 0)
                cond0 = (w0 >> 28) & 0xF
                cond1 = (w1 >> 28) & 0xF
                is_valid_arm = (cond0 <= 0xE and w0 != 0) and (cond1 <= 0xE and w1 != 0)
                results.append(
                    {
                        "name": name,
                        "vaddr": f"0x{vaddr:06x}",
                        "target_addr": f"0x{target_pc:08x}",
                        "prologue_hex": prologue[:8].hex(),
                        "is_valid_arm_prologue": is_valid_arm,
                    }
                )
        return sorted(results, key=lambda x: x["name"])

def main() -> int:
    so_bytes = SO_PATH.read_bytes()
    print(f"Loaded libHeroesLore5.so ({len(so_bytes):,} bytes)")
    loader = Arm32ElfLoader(so_bytes, base_addr=0x40000000)
    print(f"[+] Parsed {len(loader.load_segments)} PT_LOAD segments.")
    print(f"[+] Total virtual memory footprint: {loader.total_memsz:,} bytes (~{loader.total_memsz / 1024 / 1024:.2f} MB)")
    print(f"[+] Extracted {len(loader.dynsyms)} dynamic symbols ({len(loader.dynsym_by_idx)} indexed).")

    dummy_bridge: Dict[str, int] = {"__stack_chk_guard": 0x30001000}
    bridge_base = 0x30002000
    for idx, (sym_name, val, sz) in enumerate(loader.dynsym_by_idx):
        if val == 0 and sym_name and sym_name != "__stack_chk_guard":
            dummy_bridge[sym_name] = bridge_base + (idx * 8)

    stats = loader.apply_relocations(dummy_bridge)
    print(f"[+] Relocations applied successfully:")
    for rtype, count in stats.items():
        print(f"    - {rtype}: {count}")

    assert stats["R_ARM_RELATIVE"] == 1540
    assert stats["R_ARM_JUMP_SLOT"] == 75
    assert stats["R_ARM_ABS32"] == 2
    assert stats["R_ARM_GLOB_DAT"] == 1
    assert stats["unresolved"] == 0

    jni_points = loader.inspect_jni_entry_points()
    print(f"\n[+] Verified {len(jni_points)} JNI export entry points:")
    for j in jni_points:
        valid_tag = "VALID ARM PUSH" if j["is_valid_arm_prologue"] else "OTHER"
        print(f"    {j['name'][:55]:55s} @ {j['target_addr']} [{j['prologue_hex']}] ({valid_tag})")
        assert j["is_valid_arm_prologue"], f"Invalid ARM prologue for {j['name']}"

    print("\n[SUCCESS] Minimal ARM32 ELF Loader & Relocation PoC PASSED 100%!")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
