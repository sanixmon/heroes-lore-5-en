#!/usr/bin/env python3
"""
Test Suite for ARM32 Instruction Execution Engine (poc/test_execution_engine.py)

Tests:
  1. Controlled ARM32 test fixture (poc/fixtures/test_arm32_fixture.so):
     - Arithmetic & bitwise logic
     - Condition-code evaluation & loops
     - Memory loads/stores & stack frame management
     - Global data access & R_ARM_GLOB_DAT / R_ARM_ABS32 relocations
     - Indirect calls via function pointers
     - Host PLT trampoline interception & callback resolution
  2. Actual Heroes Lore 5 engine binary (apktool_modern/lib/armeabi/libHeroesLore5.so):
     - MIDASKernelManager::hash(unsigned char*) [0x1516a4] -> verified against known DJB2 hashes
     - StaticUtil::ByteToInt16(unsigned char*, int) [0x1437ec] -> verified against little-endian data
     - StaticUtil::ByteToInt32(unsigned char*, int) [0x14379c] -> verified against 32-bit data
     - BFont::IsEscape(char) [0x43668] -> verified against escape table
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

from poc.arm32_execution_engine import Arm32ExecutionEngine


def test_fixture_execution() -> None:
    print("==================================================================")
    print("STAGE 1: Controlled ARM32 Test Fixture Execution")
    print("==================================================================")
    fixture_path = ROOT_DIR / "poc" / "fixtures" / "test_arm32_fixture.so"
    engine = Arm32ExecutionEngine(fixture_path, base_addr=0x20000000)

    # Register host callback hook for host_bridge_multiply
    def host_mult(args: list[int]) -> int:
        x, factor = args[0], args[1]
        print(f"    [HOST HOOK] host_bridge_multiply called with x={x}, factor={factor}")
        return x * factor

    engine.register_host_hook("host_bridge_multiply", host_mult)

    # 1. Arithmetic & bitwise: test_arithmetic(10, 20)
    # (10+20)*(10-20) + (((10^20) | (10 & 0x0F)) >> 1) = -300 + (30 >> 1) = -285
    res1 = engine.call_function("test_arithmetic", 10, 20)
    # Convert unsigned 32-bit to signed
    if res1 >= 0x80000000:
        res1 -= 0x100000000
    expected1 = (10 + 20) * (10 - 20) + (((10 ^ 20) | (10 & 0x0F)) >> 1)
    print(f"[+] test_arithmetic(10, 20) = {res1} (expected {expected1})")
    assert res1 == expected1, f"Arithmetic mismatch: {res1} != {expected1}"

    # 2. Conditionals, CPSR flags, and loops: test_conditionals_and_loops(10)
    # acc = 0
    # i=0 (even): acc += 0 = 0
    # i=1 (odd):  acc -= 1 = -1
    # i=2 (even): acc += 4 = 3
    # i=3 (odd):  acc -= 3 = 0
    # i=4 (even): acc += 8 = 8
    # i=5 (odd):  acc -= 5 = 3
    # i=6 (even): acc += 12 = 15
    # i=7 (odd):  acc -= 7 = 8
    # i=8 (even): acc += 16 = 24
    # i=9 (odd):  acc -= 9 = 15
    res2 = engine.call_function("test_conditionals_and_loops", 10)
    print(f"[+] test_conditionals_and_loops(10) = {res2} (expected 15)")
    assert res2 == 15, f"Conditionals/loop mismatch: {res2} != 15"

    # 3. Memory load/store: test_memory_load_store(0x12345678)
    res3 = engine.call_function("test_memory_load_store", 0x12345678)
    print(f"[+] test_memory_load_store(0x12345678) = 0x{res3:08x} (expected 0x12345678)")
    assert res3 == 0x12345678, f"Memory load/store mismatch: 0x{res3:x}"

    # 4. Global data access & relocations: test_global_data(100)
    # g_magic_number (0xCAFEBABE) + 100 + ord('H') (72) = 0xCAFEBABE + 172 = 0xCAFEBB5A
    res4 = engine.call_function("test_global_data", 100)
    expected4 = (0xCAFEBABE + 100 + 72) & 0xFFFFFFFF
    print(f"[+] test_global_data(100) = 0x{res4:08x} (expected 0x{expected4:08x})")
    assert res4 == expected4, f"Global data mismatch: 0x{res4:x} != 0x{expected4:x}"

    # 5. Indirect calls: test_indirect_call(selector, x, y)
    res5_add = engine.call_function("test_indirect_call", 1, 40, 2)
    res5_sub = engine.call_function("test_indirect_call", 0, 40, 2)
    print(f"[+] test_indirect_call(1, 40, 2) = {res5_add} (expected 42)")
    print(f"[+] test_indirect_call(0, 40, 2) = {res5_sub} (expected 38)")
    assert res5_add == 42 and res5_sub == 38

    # 6. Host bridge callback via PLT: test_host_bridge(7) -> 7 * 3 = 21
    res6 = engine.call_function("test_host_bridge", 7)
    print(f"[+] test_host_bridge(7) = {res6} (expected 21)")
    assert res6 == 21, f"Host bridge mismatch: {res6} != 21"

    print("\n[PASS] All Stage 1 controlled ARM32 fixture tests PASSED 100%!\n")


def test_hl5_engine_execution() -> None:
    print("==================================================================")
    print("STAGE 2: Actual libHeroesLore5.so Native Engine Execution")
    print("==================================================================")
    so_path = ROOT_DIR / "apktool_modern" / "lib" / "armeabi" / "libHeroesLore5.so"
    engine = Arm32ExecutionEngine(so_path, base_addr=0x40000000)

    # 1. Test MIDASKernelManager::hash(unsigned char*) [symbol: _ZN18MIDASKernelManager4hashEPh]
    hash_sym = "_ZN18MIDASKernelManager4hashEPh"
    test_cases = [
        ("c/csv/name.dat", 0x53C493DA),
        ("version.txt", 0x9ABED099),
        ("c/font/kor.fnt", 0xF98A595F),
        ("c/csv/class.dat", 0x7B82C28F),
    ]
    for test_str, expected_hash in test_cases:
        str_ptr = engine.write_string(test_str)
        res_hash = engine.call_function(hash_sym, 0, str_ptr)
        print(f"[+] MIDASKernelManager::hash({test_str!r}) = 0x{res_hash:08x} (expected 0x{expected_hash:08x})")
        assert res_hash == expected_hash, f"Hash mismatch for {test_str}: 0x{res_hash:08x} != 0x{expected_hash:08x}"

    # 2. Test StaticUtil::ByteToInt16(unsigned char*, int) [symbol: _ZN10StaticUtil11ByteToInt16EPhi]
    b2i16_sym = "_ZN10StaticUtil11ByteToInt16EPhi"
    buf1 = engine.write_bytes(b"\x00\x00\x34\x12\xf0\xff\x00\x00")
    # offset 2: 0x1234 = 4660
    val_pos = engine.call_function(b2i16_sym, buf1, 2)
    print(f"[+] StaticUtil::ByteToInt16(buf, 2) = {val_pos} (expected 4660 / 0x1234)")
    assert val_pos == 4660

    # offset 4: 0xfff0 = -16
    val_neg = engine.call_function(b2i16_sym, buf1, 4)
    if val_neg >= 0x80000000:
        val_neg -= 0x100000000
    print(f"[+] StaticUtil::ByteToInt16(buf, 4) = {val_neg} (expected -16)")
    assert val_neg == -16

    # 3. Test StaticUtil::ByteToInt32(unsigned char*, int) [symbol: _ZN10StaticUtil11ByteToInt32EPhi]
    b2i32_sym = "_ZN10StaticUtil11ByteToInt32EPhi"
    buf2 = engine.write_bytes(b"\x78\x56\x34\x12")
    val_32 = engine.call_function(b2i32_sym, buf2, 0)
    print(f"[+] StaticUtil::ByteToInt32(buf, 0) = 0x{val_32:08x} (expected 0x12345678)")
    assert val_32 == 0x12345678

    # 4. Test BFont::IsEscape(char) [symbol: _ZN5BFont8IsEscapeEc]
    # Disassembly at 0x43668: r0=this (must point to BFont struct with escape table at this+0x18), r1=char
    # In BFont struct, offset 0x18 holds the 11-byte escape table.
    # Let's allocate a mock BFont object with the patched escape table at offset 0x18!
    mock_bfont = engine.write_bytes(b"\x00" * 0x18 + b"&|${}~\"`^<[\x00\x00")
    bfont_escape_sym = "_ZN5BFont8IsEscapeEc"
    is_semi = engine.call_function(bfont_escape_sym, mock_bfont, ord(";"))
    is_tilde = engine.call_function(bfont_escape_sym, mock_bfont, ord("~"))
    is_apos = engine.call_function(bfont_escape_sym, mock_bfont, ord("'"))
    is_alpha = engine.call_function(bfont_escape_sym, mock_bfont, ord("A"))
    print(f"[+] BFont::IsEscape(';') = {is_semi} (expected 1)")
    print(f"[+] BFont::IsEscape('~') = {is_tilde} (expected 1, patched yellow tag)")
    print(f"[+] BFont::IsEscape('\\'') = {is_apos} (expected 0, normal apostrophe)")
    print(f"[+] BFont::IsEscape('A') = {is_alpha} (expected 0)")
    assert is_semi == 1 and is_tilde == 1 and is_apos == 0 and is_alpha == 0

    print("\n[PASS] All Stage 2 libHeroesLore5.so native engine executions PASSED 100%!\n")


def main() -> int:
    test_fixture_execution()
    test_hl5_engine_execution()
    print("==================================================================")
    print("ALL ARM32 EXECUTION ENGINE TESTS PASSED 100%!")
    print("==================================================================")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
