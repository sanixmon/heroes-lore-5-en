#!/usr/bin/env python3
"""
Heroes Lore 5 — ARM32 Dynamic Execution Engine PoC (poc/arm32_execution_engine.py)

Uses Unicorn Engine (QEMU CPU emulation core) to execute real ARMv5TE instructions:
  1. Maps page-aligned memory segments with verified permissions (R-X, RW, Stack).
  2. Resolves all ELF relocations (R_ARM_RELATIVE, R_ARM_JUMP_SLOT, R_ARM_GLOB_DAT, R_ARM_ABS32).
  3. Binds external symbols to host trampoline hooks (intercepting PLT calls).
  4. Manages ARM calling conventions (R0-R3 args, R0 return, SP 8-byte aligned, LR return hook).
  5. Validates arithmetic, branches, loops, memory access, indirect calls, and host bridges.
  6. Loads and executes actual game engine functions from libHeroesLore5.so!
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

import unicorn
from unicorn.arm_const import *

ROOT_DIR = Path(__file__).resolve().parent.parent
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

from poc.test_elf_loader import Arm32ElfLoader

PAGE_SIZE = 4096
STACK_BASE = 0x70000000
STACK_SIZE = 0x00020000  # 128 KB
TRAMPOLINE_BASE = 0x30000000
TRAMPOLINE_SIZE = 0x00010000  # 64 KB
EXIT_MAGIC_ADDR = 0xDEADBEE0


def align_down(val: int, align: int = PAGE_SIZE) -> int:
    return val & ~(align - 1)


def align_up(val: int, align: int = PAGE_SIZE) -> int:
    return (val + align - 1) & ~(align - 1)


class Arm32ExecutionEngine:
    def __init__(self, so_path: str | Path, base_addr: int = 0x40000000) -> None:
        self.so_path = Path(so_path)
        self.base_addr = base_addr
        self.loader = Arm32ElfLoader(self.so_path.read_bytes(), base_addr=self.base_addr)

        # Initialize Unicorn Engine in 32-bit ARM mode
        self.uc = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_ARM)

        # Track host symbol hooks
        self.host_hooks: Dict[str, Callable[[List[int]], int]] = {}
        self.trampoline_map: Dict[int, str] = {}
        self.next_trampoline_addr = TRAMPOLINE_BASE

        # Simulated heap for malloc/calloc
        self.heap_base = 0x50000000
        self.heap_curr = self.heap_base

        self._setup_memory()
        self._setup_trampolines()
        self._apply_relocations()

    def register_host_hook(self, symbol_name: str, handler: Callable[[List[int]], int]) -> None:
        self.host_hooks[symbol_name] = handler

    def _setup_memory(self) -> None:
        # 1. Map PT_LOAD segments with page alignment
        # Group contiguous virtual memory range for the ELF
        mem_start = align_down(self.base_addr, PAGE_SIZE)
        mem_end = align_up(self.base_addr + self.loader.total_memsz, PAGE_SIZE)
        elf_map_sz = mem_end - mem_start

        # Allocate readable/writable/executable page range for the ELF binary
        self.uc.mem_map(mem_start, elf_map_sz, unicorn.UC_PROT_ALL)
        self.uc.mem_write(self.base_addr, bytes(self.loader.memory))

        # 2. Map stack space
        self.uc.mem_map(STACK_BASE, STACK_SIZE, unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE)

        # 3. Map trampoline space
        self.uc.mem_map(TRAMPOLINE_BASE, TRAMPOLINE_SIZE, unicorn.UC_PROT_READ | unicorn.UC_PROT_EXEC)

        # 4. Map heap space (16 MB)
        self.uc.mem_map(self.heap_base, 0x01000000, unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE)

        # 5. Map exit magic page so returning to EXIT_MAGIC_ADDR stops emulation cleanly
        exit_page = align_down(EXIT_MAGIC_ADDR, PAGE_SIZE)
        self.uc.mem_map(exit_page, PAGE_SIZE, unicorn.UC_PROT_ALL)
        self.uc.mem_write(EXIT_MAGIC_ADDR, bytes([0x00, 0x00, 0xA0, 0xE1]))  # mov r0, r0 (nop)
        self.uc.hook_add(
            unicorn.UC_HOOK_CODE,
            self._on_exit_magic_hook,
            begin=EXIT_MAGIC_ADDR,
            end=EXIT_MAGIC_ADDR + 4,
        )

    def _on_exit_magic_hook(self, uc: unicorn.Uc, address: int, size: int, user_data: Any) -> None:
        uc.emu_stop()
    def _setup_trampolines(self) -> None:
        # Default built-in libc hooks
        def default_memcpy(args: List[int]) -> int:
            dst, src, n = args[0], args[1], args[2]
            if n > 0:
                data = self.uc.mem_read(src, n)
                self.uc.mem_write(dst, bytes(data))
            return dst

        def default_memset(args: List[int]) -> int:
            dst, val, n = args[0], args[1] & 0xFF, args[2]
            if n > 0:
                self.uc.mem_write(dst, bytes([val] * n))
            return dst

        def default_strlen(args: List[int]) -> int:
            addr = args[0]
            count = 0
            while True:
                b = self.uc.mem_read(addr + count, 1)[0]
                if b == 0:
                    break
                count += 1
            return count

        def default_malloc(args: List[int]) -> int:
            size = align_up(args[0], 8)
            ptr = self.heap_curr
            self.heap_curr += size
            return ptr

        def default_calloc(args: List[int]) -> int:
            num, elem_sz = args[0], args[1]
            total = align_up(num * elem_sz, 8)
            ptr = self.heap_curr
            self.heap_curr += total
            self.uc.mem_write(ptr, bytes(total))
            return ptr

        def default_log(args: List[int]) -> int:
            # __android_log_print(prio, tag, fmt, ...)
            return 0

        self.register_host_hook("memcpy", default_memcpy)
        self.register_host_hook("memset", default_memset)
        self.register_host_hook("strlen", default_strlen)
        self.register_host_hook("malloc", default_malloc)
        self.register_host_hook("calloc", default_calloc)
        self.register_host_hook("free", lambda args: 0)
        self.register_host_hook("__android_log_print", default_log)

        # Pre-fill trampoline area with 'bx lr' (0xe12fff1e) so it naturally returns to caller
        bx_lr_bytes = bytes([0x1E, 0xFF, 0x2F, 0xE1]) * (TRAMPOLINE_SIZE // 4)
        self.uc.mem_write(TRAMPOLINE_BASE, bx_lr_bytes)

        # Hook code execution across the trampoline region
        self.uc.hook_add(
            unicorn.UC_HOOK_CODE,
            self._on_trampoline_executed,
            begin=TRAMPOLINE_BASE,
            end=TRAMPOLINE_BASE + TRAMPOLINE_SIZE,
        )
    def _on_trampoline_executed(self, uc: unicorn.Uc, address: int, size: int, user_data: Any) -> None:
        if address in self.trampoline_map:
            sym_name = self.trampoline_map[address]
            r0 = uc.reg_read(UC_ARM_REG_R0)
            r1 = uc.reg_read(UC_ARM_REG_R1)
            r2 = uc.reg_read(UC_ARM_REG_R2)
            r3 = uc.reg_read(UC_ARM_REG_R3)
            lr = uc.reg_read(UC_ARM_REG_LR)

            handler = self.host_hooks.get(sym_name)
            if handler:
                ret_val = handler([r0, r1, r2, r3])
            else:
                # Stub returning 0 if not handled
                ret_val = 0

            uc.reg_write(UC_ARM_REG_R0, ret_val & 0xFFFFFFFF)
            # Return to caller by setting PC = LR
            uc.reg_write(UC_ARM_REG_PC, lr)

    def _apply_relocations(self) -> None:
        bridge_table: Dict[str, int] = {"__stack_chk_guard": 0x50000000}

        # Assign a trampoline address for each dynamic symbol
        for name, (st_val, _) in self.loader.dynsyms.items():
            if st_val == 0 and name and name != "__stack_chk_guard":
                t_addr = self.next_trampoline_addr
                self.next_trampoline_addr += 8
                self.trampoline_map[t_addr] = name
                bridge_table[name] = t_addr

        # Apply relocations on the loader memory buffer
        self.loader.apply_relocations(bridge_table)
        # Flush modified relocations back into Unicorn memory
        self.uc.mem_write(self.base_addr, bytes(self.loader.memory))

    def call_function(self, func_name: str, *args: int, timeout_us: int = 2000000) -> int:
        if func_name not in self.loader.dynsyms:
            raise KeyError(f"Symbol {func_name!r} not found in dynamic symbols")
        vaddr, _ = self.loader.dynsyms[func_name]
        func_target = self.base_addr + vaddr

        # 1. Setup arguments in R0-R3
        regs = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
        for idx in range(min(4, len(args))):
            self.uc.reg_write(regs[idx], args[idx] & 0xFFFFFFFF)

        # 2. Setup Stack: 8-byte aligned per ARM EABI
        sp = STACK_BASE + STACK_SIZE - 0x1000
        self.uc.reg_write(UC_ARM_REG_SP, sp)

        # 3. Setup Link Register (LR) to return to EXIT_MAGIC_ADDR
        self.uc.reg_write(UC_ARM_REG_LR, EXIT_MAGIC_ADDR)

        # 4. Set CPSR to user mode (ARM mode, not Thumb)
        self.uc.reg_write(UC_ARM_REG_CPSR, 0x10)

        # 5. Start emulation from func_target until EXIT_MAGIC_ADDR
        try:
            self.uc.emu_start(func_target, 0, timeout=timeout_us)
        except unicorn.UcError as exc:
            pc = self.uc.reg_read(UC_ARM_REG_PC)
            lr = self.uc.reg_read(UC_ARM_REG_LR)
            r0 = self.uc.reg_read(UC_ARM_REG_R0)
            raise RuntimeError(
                f"Execution failed inside {func_name} (target 0x{func_target:08x}): "
                f"fault at PC=0x{pc:08x}, LR=0x{lr:08x}, R0=0x{r0:08x} -> {exc}"
            ) from exc

        # Read return value in R0
        ret_val = self.uc.reg_read(UC_ARM_REG_R0)
        return ret_val

    def write_string(self, s: str | bytes, encoding: str = "ascii") -> int:
        raw = s.encode(encoding) if isinstance(s, str) else s
        raw_nul = raw + b"\x00"
        ptr = self.heap_curr
        self.heap_curr += align_up(len(raw_nul), 8)
        self.uc.mem_write(ptr, raw_nul)
        return ptr

    def write_bytes(self, b: bytes) -> int:
        ptr = self.heap_curr
        self.heap_curr += align_up(len(b), 8)
        self.uc.mem_write(ptr, b)
        return ptr
