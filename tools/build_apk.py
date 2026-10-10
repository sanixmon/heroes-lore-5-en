#!/usr/bin/env python3
"""
Heroes Lore 5 — English APK Build Pipeline (tools/build_apk.py)

Steps:
  1. Load validated translations from translations/unique_strings.json & catalog.json.
  2. Compile all 57 CSV .dat files with updated row sizes and string lengths.
  3. Compile all 258 .scn files with updated string tables (auto-upgrading to uint16_le where needed).
  4. Repack assets/data.vfs.mp3 deterministically, keeping existing media and updating translated text.
  5. Apply verified 1-byte BFont escape patch to lib/armeabi/libHeroesLore5.so:
       Offset 0x157965: 0x27 ('\'') -> 0x7e ('~') so English apostrophe renders normally.
  6. Update res/values/strings.xml (app_name -> "Heroes Lore 5").
  7. Rebuild APK using apktool (ensuring data.vfs.mp3 is STORED uncompressed for openFd).
  8. Zipalign aligned to 4-byte boundary.
  9. Sign with debug keystore using apksigner.
 10. Verify APK signature and output build summary report.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path
from typing import Any, Dict, List, Optional

ROOT_DIR = Path(__file__).resolve().parent.parent
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

from tools.hl5_formats import (
    compile_csv_dat,
    compile_scn_file,
    is_scn_file,
    is_text_csv_file,
    parse_csv_dat,
    parse_scn_file,
)
from tools.hl5_vfs import VfsArchive

SO_ESCAPE_TABLE_OFFSET = 0x157965  # Offset of '\'' (0x27) in libHeroesLore5.so


def patch_native_library(so_path: Path, dry_run: bool = False) -> Dict[str, Any]:
    raw = bytearray(so_path.read_bytes())
    cur_byte = raw[SO_ESCAPE_TABLE_OFFSET]
    if cur_byte == 0x27:  # '\''
        raw[SO_ESCAPE_TABLE_OFFSET] = 0x7E  # '~'
        if not dry_run:
            so_path.write_bytes(raw)
        return {
            "status": "patched",
            "offset": f"0x{SO_ESCAPE_TABLE_OFFSET:x}",
            "original_byte": "0x27 ('\\'')",
            "new_byte": "0x7e ('~')",
            "sha256": hashlib.sha256(raw).hexdigest(),
        }
    elif cur_byte == 0x7E:
        return {
            "status": "already_patched",
            "offset": f"0x{SO_ESCAPE_TABLE_OFFSET:x}",
            "byte": "0x7e ('~')",
            "sha256": hashlib.sha256(raw).hexdigest(),
        }
    else:
        raise ValueError(
            f"Unexpected byte at 0x{SO_ESCAPE_TABLE_OFFSET:x}: 0x{cur_byte:02x} "
            f"(expected 0x27 or 0x7e)"
        )


def build_english_vfs(
    base_vfs_path: Path,
    catalog_path: Path,
    unique_path: Path,
    overrides_path: Optional[Path] = None,
) -> Tuple[bytes, Dict[str, Any]]:
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    unique_data = json.loads(unique_path.read_text(encoding="utf-8"))
    unique_by_src = {item["source"]: item for item in unique_data.get("strings", [])}

    slot_overrides: Dict[str, str] = {}
    if overrides_path and overrides_path.exists():
        slot_overrides = json.loads(overrides_path.read_text(encoding="utf-8"))

    slots_by_file: Dict[str, List[Tuple[Dict[str, Any], str]]] = {}
    translated_count = 0

    for slot in catalog["slots"]:
        sid = slot["id"]
        src = slot["source"]
        if sid in slot_overrides and slot_overrides[sid]:
            text = slot_overrides[sid]
        elif src in unique_by_src and unique_by_src[src].get("translation"):
            text = unique_by_src[src]["translation"]
        else:
            text = src
        if text != src:
            translated_count += 1
        slots_by_file.setdefault(slot["file"], []).append((slot, text))

    vfs = VfsArchive.from_file(base_vfs_path, verify=True)
    overrides: Dict[str, bytes] = {}
    csv_compiled = 0
    scn_compiled = 0

    for entry in vfs.entries:
        fname = entry.name
        if is_text_csv_file(fname):
            orig_rows = parse_csv_dat(fname, entry.data)
            new_rows = [list(r) for r in orig_rows]
            for slot, text in slots_by_file.get(fname, []):
                new_rows[slot["row_index"]][slot["field_index"]] = text
            compiled_csv = compile_csv_dat(fname, entry.data, new_rows)
            overrides[fname] = compiled_csv
            csv_compiled += 1

        elif is_scn_file(fname):
            scn = parse_scn_file(fname, entry.data)
            new_strs = list(scn.strings)
            for slot, text in slots_by_file.get(fname, []):
                new_strs[slot["row_index"]] = text
            compiled_scn = compile_scn_file(scn, new_strs)
            overrides[fname] = compiled_scn
            scn_compiled += 1

    repacked_vfs = vfs.to_bytes(overrides)
    stats = {
        "csv_compiled": csv_compiled,
        "scn_compiled": scn_compiled,
        "translated_slots_applied": translated_count,
        "repacked_vfs_size": len(repacked_vfs),
        "repacked_vfs_sha256": hashlib.sha256(repacked_vfs).hexdigest(),
    }
    return repacked_vfs, stats


def update_android_strings(xml_path: Path, app_name: str = "Heroes Lore 5") -> None:
    if not xml_path.exists():
        return
    tree = ET.parse(xml_path)
    root = tree.getroot()
    for elem in root.findall("string"):
        if elem.attrib.get("name") == "app_name":
            elem.text = app_name
    tree.write(xml_path, encoding="utf-8", xml_declaration=True)


def build_and_sign_apk(
    apktool_dir: Path,
    out_apk_path: Path,
    keystore_path: Path,
    key_alias: str = "hl5test",
    key_pass: str = "hl5testpass",
) -> Dict[str, Any]:
    build_dir = out_apk_path.parent
    build_dir.mkdir(parents=True, exist_ok=True)
    unsigned_apk = build_dir / "hl5-en-unsigned.apk"
    aligned_apk = build_dir / "hl5-en-aligned.apk"

    # 1. Rebuild with apktool
    print(f"[*] Rebuilding APK with apktool from {apktool_dir}...")
    cmd_b = ["apktool", "b", str(apktool_dir), "-o", str(unsigned_apk)]
    res_b = subprocess.run(cmd_b, capture_output=True, text=True)
    if res_b.returncode != 0:
        raise RuntimeError(f"apktool build failed:\n{res_b.stderr}\n{res_b.stdout}")

    # 2. Verify assets/data.vfs.mp3 is STORED (uncompressed) in ZIP
    with zipfile.ZipFile(unsigned_apk, "r") as zf:
        vfs_info = zf.getinfo("assets/data.vfs.mp3")
        if vfs_info.compress_type != zipfile.ZIP_STORED:
            raise ValueError(
                f"assets/data.vfs.mp3 is compressed (compress_type={vfs_info.compress_type}). "
                f"Must be STORED (0) for AssetManager.openFd!"
            )
        print(f"[+] Verified assets/data.vfs.mp3 is STORED uncompressed ({vfs_info.file_size:,} bytes).")

    # 3. Zipalign aligned to 4-byte boundaries
    print("[*] Running zipalign...")
    if aligned_apk.exists():
        aligned_apk.unlink()
    cmd_z = ["zipalign", "-f", "-p", "4", str(unsigned_apk), str(aligned_apk)]
    res_z = subprocess.run(cmd_z, capture_output=True, text=True)
    if res_z.returncode != 0:
        raise RuntimeError(f"zipalign failed:\n{res_z.stderr}\n{res_z.stdout}")

    # 4. Generate debug test keystore if not exists
    if not keystore_path.exists():
        print(f"[*] Generating test keystore at {keystore_path}...")
        keystore_path.parent.mkdir(parents=True, exist_ok=True)
        cmd_k = [
            "keytool",
            "-genkeypair",
            "-v",
            "-keystore",
            str(keystore_path),
            "-storepass",
            key_pass,
            "-alias",
            key_alias,
            "-keypass",
            key_pass,
            "-keyalg",
            "RSA",
            "-keysize",
            "2048",
            "-validity",
            "10000",
            "-dname",
            "CN=HL5 English, OU=Translation, O=Community, L=Retro, ST=Retro, C=KR",
        ]
        res_k = subprocess.run(cmd_k, capture_output=True, text=True)
        if res_k.returncode != 0:
            raise RuntimeError(f"keytool failed:\n{res_k.stderr}\n{res_k.stdout}")

    # 5. Sign with apksigner
    print("[*] Signing APK with apksigner...")
    if out_apk_path != aligned_apk:
        shutil.copy2(aligned_apk, out_apk_path)
    cmd_s = [
        "apksigner",
        "sign",
        "--ks",
        str(keystore_path),
        "--ks-pass",
        f"pass:{key_pass}",
        "--ks-key-alias",
        key_alias,
        "--key-pass",
        f"pass:{key_pass}",
        str(out_apk_path),
    ]
    res_s = subprocess.run(cmd_s, capture_output=True, text=True)
    if res_s.returncode != 0:
        raise RuntimeError(f"apksigner sign failed:\n{res_s.stderr}\n{res_s.stdout}")

    # 6. Verify signature
    cmd_v = ["apksigner", "verify", "--verbose", str(out_apk_path)]
    res_v = subprocess.run(cmd_v, capture_output=True, text=True)
    if res_v.returncode != 0:
        raise RuntimeError(f"apksigner verify failed:\n{res_v.stderr}\n{res_v.stdout}")

    final_bytes = out_apk_path.read_bytes()
    return {
        "apk_path": str(out_apk_path),
        "size": len(final_bytes),
        "sha256": hashlib.sha256(final_bytes).hexdigest(),
        "apksigner_verification": res_v.stdout.strip(),
    }


def run_full_build(
    apktool_dir: Path = Path("apktool_modern"),
    out_apk_path: Path = Path("hl5-en.apk"),
    catalog_path: Path = Path("translations/catalog.json"),
    unique_path: Path = Path("translations/unique_strings.json"),
    overrides_path: Optional[Path] = Path("translations/slot_overrides.json"),
    keystore_path: Path = Path("build/test.keystore"),
    report_path: Optional[Path] = Path("build/build_report.json"),
) -> Dict[str, Any]:
    print("[1/5] Building English VFS archive with translated resources...")
    base_vfs = apktool_dir / "assets" / "data.vfs.mp3"
    repacked_vfs_bytes, vfs_stats = build_english_vfs(
        base_vfs, catalog_path, unique_path, overrides_path
    )
    base_vfs.write_bytes(repacked_vfs_bytes)
    print(f"[+] Replaced {base_vfs} ({len(repacked_vfs_bytes):,} bytes, SHA256={vfs_stats['repacked_vfs_sha256'][:16]}...)")

    print("[2/5] Patching BFont yellow color escape in all lib/*/libHeroesLore5.so copies...")
    patch_stats = None
    for so_file in apktool_dir.glob("lib/*/libHeroesLore5.so"):
        patch_stats = patch_native_library(so_file)
        print(f"[+] Native patch on {so_file.parent.name}: {patch_stats['status']} at {patch_stats['offset']}")

    print("[3/5] Updating Android strings (app_name)...")
    strings_xml = apktool_dir / "res" / "values" / "strings.xml"
    update_android_strings(strings_xml, "Heroes Lore 5")

    print("[4/5] Rebuilding, aligning, and signing target APK...")
    apk_stats = build_and_sign_apk(apktool_dir, out_apk_path, keystore_path)
    print(f"[+] Target APK built: {apk_stats['apk_path']} ({apk_stats['size']:,} bytes)")

    report = {
        "status": "success",
        "output_apk": apk_stats,
        "vfs_stats": vfs_stats,
        "native_patch": patch_stats,
    }
    if report_path:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"[5/5] Build report saved to {report_path}")

    return report


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Heroes Lore 5 English APK Build Pipeline")
    parser.add_argument("--apktool-dir", default="apktool_modern")
    parser.add_argument("-o", "--out", default="hl5-en.apk")
    parser.add_argument("--catalog", default="translations/catalog.json")
    parser.add_argument("--unique", default="translations/unique_strings.json")
    parser.add_argument("--overrides", default="translations/slot_overrides.json")
    parser.add_argument("--keystore", default="build/test.keystore")
    parser.add_argument("--report", default="build/build_report.json")
    args = parser.parse_args(argv)

    res = run_full_build(
        apktool_dir=Path(args.apktool_dir),
        out_apk_path=Path(args.out),
        catalog_path=Path(args.catalog),
        unique_path=Path(args.unique),
        overrides_path=Path(args.overrides) if Path(args.overrides).exists() else None,
        keystore_path=Path(args.keystore),
        report_path=Path(args.report),
    )
    print(json.dumps(res, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
