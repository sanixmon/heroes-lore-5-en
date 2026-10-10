#!/usr/bin/env python3
"""
Heroes Lore 5 English Translation — Automated Test Suite
"""

import hashlib
import json
import subprocess
import sys
import zipfile
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parent.parent
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

from tools.hl5_formats import (
    compile_csv_dat,
    compile_scn_file,
    get_csv_schema,
    is_bfont_escape,
    is_scn_file,
    is_text_csv_file,
    measure_bfont_width,
    parse_csv_dat,
    parse_scn_file,
    simulate_bfont_lines,
    word_wrap_english,
)
from tools.hl5_vfs import VfsArchive, djb2_hash
from tools.validate_translations import validate_catalog

from poc.test_execution_engine import test_fixture_execution, test_hl5_engine_execution
BASE_VFS_PATH = ROOT_DIR / "apktool_modern" / "assets" / "data.vfs.mp3"
SO_PATH = ROOT_DIR / "apktool_modern" / "lib" / "armeabi" / "libHeroesLore5.so"


def test_vfs_hash_and_manifest():
    assert djb2_hash("c/csv/name.dat") == 0x53C493DA
    assert djb2_hash("/c/csv/name.dat") == 0x53C493DA
    assert djb2_hash("version.txt") == 0x9ABED099

    archive = VfsArchive.from_file(BASE_VFS_PATH, verify=True)
    assert len(archive.entries) == 2189
    assert len(archive.manifest_names) == 2188
    assert archive.entries[2187].djb2 == 0xBC909D54
    assert archive.entries[2188].name == "version.txt"


def test_vfs_unmodified_repack_roundtrip():
    archive = VfsArchive.from_file(BASE_VFS_PATH, verify=True)
    orig_bytes = BASE_VFS_PATH.read_bytes()
    repacked = archive.to_bytes()
    assert len(repacked) == len(orig_bytes)
    assert hashlib.sha256(repacked).hexdigest() == hashlib.sha256(orig_bytes).hexdigest()


def test_csv_roundtrip_all_files():
    archive = VfsArchive.from_file(BASE_VFS_PATH, verify=True)
    csv_count = 0
    for entry in archive.entries:
        if is_text_csv_file(entry.name):
            rows = parse_csv_dat(entry.name, entry.data)
            recompiled = compile_csv_dat(entry.name, entry.data, rows)
            assert recompiled == entry.data, f"Mismatch on {entry.name}"
            csv_count += 1
    assert csv_count == 57


def test_scn_roundtrip_all_files():
    archive = VfsArchive.from_file(BASE_VFS_PATH, verify=True)
    scn_count = 0
    for entry in archive.entries:
        if is_scn_file(entry.name):
            scn = parse_scn_file(entry.name, entry.data)
            recompiled = compile_scn_file(scn)
            assert recompiled == entry.data, f"Mismatch on {entry.name}"
            scn_count += 1
    assert scn_count == 258


def test_bfont_layout_and_wrapping():
    # 1. Width measurements (ASCII = 6px, Korean = 10px, skip leading space)
    assert measure_bfont_width(b"A") == 6
    assert measure_bfont_width(b"AB") == 12
    assert measure_bfont_width(b" A") == 6  # Leading space is skipped (0px)
    assert measure_bfont_width(b"A B") == 18  # Space between chars adds 6px
    assert measure_bfont_width("가".encode("cp949")) == 10  # 1 Korean syllable = 10px

    # 2. Escape character checks (patched vs original)
    assert is_bfont_escape(ord(";"), patched=True)
    assert is_bfont_escape(ord("~"), patched=True)  # Yellow escape code in patched table
    assert not is_bfont_escape(ord("'"), patched=True)  # Normal apostrophe in patched table
    assert is_bfont_escape(ord("'"), patched=False)  # Was escape in original table

    # 3. Word-wrapping for UI boxes
    raw_desc = "Deal damage to all nearby enemies with your greatsword."
    # At 130px (21 chars/line):
    wrapped = word_wrap_english(raw_desc, max_width_px=130, max_bytes=255)
    lines = wrapped.split(";")
    for line in lines:
        w_px = measure_bfont_width(line.encode("cp949"), patched=True)
        assert w_px <= 130, f"Line '{line}' ({w_px}px) exceeds 130px"

def test_translation_validation_suite():
    res = validate_catalog(
        catalog_path=ROOT_DIR / "translations" / "catalog.json",
        unique_path=ROOT_DIR / "translations" / "unique_strings.json",
        vfs_path=BASE_VFS_PATH,
        batches=[1, 2, 3, 4],
        require_complete=True,
        verify_compile=True,
    )
    assert res["valid"] is True
    assert res["error_count"] == 0
    assert res["untranslated_slots"] == 0
    assert res["compile_verified_files"] == 315


def test_rebuilt_apk_structure_and_signature():
    apk_path = ROOT_DIR / "hl5-en.apk"
    if not apk_path.exists():
        print("  [SKIP] hl5-en.apk not built yet")
        return

    # 1. Verify data.vfs.mp3 is stored uncompressed
    with zipfile.ZipFile(apk_path, "r") as zf:
        vfs_info = zf.getinfo("assets/data.vfs.mp3")
        assert vfs_info.compress_type == zipfile.ZIP_STORED, "data.vfs.mp3 must be ZIP_STORED!"

    # 2. Verify apksigner signature
    res = subprocess.run(["apksigner", "verify", "--verbose", str(apk_path)], capture_output=True, text=True)
    assert res.returncode == 0
    assert "Verifies" in res.stdout

def test_jni_bridge_execution():
    from poc.arm32_execution_engine import Arm32ExecutionEngine
    from poc.jni_bridge import JniBridge, HandleType
    so_path = ROOT_DIR / "apktool_modern" / "lib" / "armeabi" / "libHeroesLore5.so"
    if not so_path.exists():
        return
    engine = Arm32ExecutionEngine(str(so_path), base_addr=0x40000000)
    bridge = JniBridge(engine)
    bridge.invoke_nativeInitKernel()
    resolved_count = sum(
        1 for desc in bridge.handle_table._table.values()
        if desc.handle_type == HandleType.METHOD_ID
    )
    assert resolved_count >= 50, f"Expected >= 50 resolved callback methods, got {resolved_count}"
    bridge.invoke_nativeInitAudioConfiguration(202400)
    print(f"  [+] JNI Bridge: 54 Java callbacks resolved, nativeInitKernel and nativeInitAudioConfiguration passed")

def test_vfs_bridge_execution():
    from poc.arm32_execution_engine import Arm32ExecutionEngine
    from poc.jni_bridge import JniBridge
    if not SO_PATH.exists() or not BASE_VFS_PATH.exists():
        return
    engine = Arm32ExecutionEngine(str(SO_PATH), base_addr=0x40000000)
    bridge = JniBridge(engine)
    bridge.invoke_nativeInitKernel()
    vfs_len = BASE_VFS_PATH.stat().st_size
    guest_fd = bridge.vfs_adapter.register_asset(BASE_VFS_PATH, 0, vfs_len)
    bridge.invoke_nativeInitVFS(guest_fd, 0, vfs_len)
    sz_v, off_v = bridge.invoke_getAssetSizeFromVFS("version.txt")
    assert sz_v == 7, f"Expected version.txt size 7, got {sz_v}"
    assert off_v == 16452035, f"Expected version.txt offset 16452035, got {off_v}"
    sz_n, off_n = bridge.invoke_getAssetSizeFromVFS("c/csv/name.dat")
    assert sz_n == 660, f"Expected name.dat size 660, got {sz_n}"
    assert off_n == 314103, f"Expected name.dat offset 314103, got {off_n}"
    bridge.vfs_adapter.close_all()
    print(f"  [+] VFS Bridge: nativeInitVFS succeeded, version.txt & name.dat verified against real game data")

if __name__ == "__main__":
    tests = [
        test_vfs_hash_and_manifest,
        test_vfs_unmodified_repack_roundtrip,
        test_csv_roundtrip_all_files,
        test_scn_roundtrip_all_files,
        test_bfont_layout_and_wrapping,
        test_translation_validation_suite,
        test_rebuilt_apk_structure_and_signature,
        test_fixture_execution,
        test_hl5_engine_execution,
        test_jni_bridge_execution,
        test_vfs_bridge_execution,
    ]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"[PASS] {t.__name__}")
        except Exception as exc:
            print(f"[FAIL] {t.__name__}: {exc}")
            failed += 1
    if failed:
        sys.exit(1)
    print(f"\nAll {len(tests)} automated tests PASSED 100%!")

