#!/usr/bin/env python3
"""
Heroes Lore 5 — String Extractor & Translation Catalog Builder (tools/extract_strings.py)

Extracts all player-facing strings from:
  - 57 text-bearing CSV .dat files in c/csv/*.dat and c/csv2/*.dat (7,814 slots)
  - 258 scenario/event .scn files in c/ep/*/*.scn and c/map/*.scn (7,425 slots)
  - Android resource strings in apktool_modern/res/values/strings.xml (2 slots)

Produces:
  - translations/catalog.json: Full slot-level inventory (15,241 slots) with stable IDs,
    resource context, max_bytes, max_width_px, batch number, and protected tokens.
  - translations/unique_strings.json: Deduplicated translation table grouped by
    (source, max_bytes_bucket) or preserved in existing translation files.
"""

from __future__ import annotations

import argparse
import json
import sys
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Dict, List, Optional

ROOT_DIR = Path(__file__).resolve().parent.parent
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

from tools.hl5_formats import (
    HANGUL_RE,
    extract_tokens,
    get_csv_schema,
    is_scn_file,
    is_text_csv_file,
    parse_csv_dat,
    parse_scn_file,
)
from tools.hl5_vfs import VfsArchive


def infer_scn_string_constraints(scn_path: str, str_idx: int, title_idx: int, text: str) -> Tuple[int, Optional[int], str]:
    """
    Determine (max_bytes, max_width_px, context_note) for a .scn string slot.
    - If str_idx == title_idx: copied into [map + 4225] (char[52] buffer in EventProc::Scene_Init @ 0x72a88 -> max 51 bytes!).
    - If text starts with '^' or is short choice/popup: Event_SituatePopup (156px) or onMessageBox (150px).
    - Otherwise: Event_SituateDialogText (384px, 3 lines/page) or Event_SituateNarration (220px).
      Note: since .scn header b[2] (strLenType) can be upgraded from 1 (uint8) to 2 (uint16_le)
      when needed, dialogue strings can safely be up to 512 bytes, except map title strings which
      are copied into a 52-byte buffer (max 51 bytes).
    """
    if str_idx == title_idx:
        return 51, None, "scene_map_title"
    if scn_path.startswith("c/ep/"):
        return 254, 220, "episode_script"
    return 512, 360, "dialogue_or_event"


def extract_all(
    vfs_path: Path,
    strings_xml_path: Path,
    out_dir: Path,
    preserve_existing: bool = True,
) -> Dict[str, Any]:
    vfs = VfsArchive.from_file(vfs_path, verify=True)

    slots: List[Dict[str, Any]] = []
    file_counts = Counter()
    batch_slot_counts = Counter()

    # 1. Extract from Android res/values/strings.xml
    if strings_xml_path.exists():
        tree = ET.parse(strings_xml_path)
        root = tree.getroot()
        for idx, elem in enumerate(root.findall("string")):
            name = elem.attrib.get("name", f"str_{idx}")
            src_text = elem.text or ""
            slot_id = f"res/values/strings.xml#{name}"
            slots.append(
                {
                    "id": slot_id,
                    "file": "res/values/strings.xml",
                    "row_index": idx,
                    "field_index": 0,
                    "field_name": name,
                    "source": src_text,
                    "max_bytes": 120,
                    "max_width_px": None,
                    "batch": 1,
                    "tokens": [],
                    "context": f"Android manifest/resource string ({name})",
                }
            )
            file_counts["res/values/strings.xml"] += 1
            batch_slot_counts[1] += 1

    # 2. Extract from all 57 text-bearing CSV .dat files
    csv_files_count = 0
    for entry in vfs.entries:
        if not is_text_csv_file(entry.name):
            continue
        csv_files_count += 1
        _, specs = get_csv_schema(entry.name)
        rows = parse_csv_dat(entry.name, entry.data)
        for r_idx, row_strs in enumerate(rows):
            for f_idx, src_text in enumerate(row_strs):
                spec = specs[f_idx]
                slot_id = f"{entry.name}#{r_idx}:{spec.field_name}"
                # Add sibling field context if row has multiple strings (e.g., item name for item description)
                ctx_parts = [f"{entry.name} row {r_idx} ({spec.field_name})"]
                if len(row_strs) > 1 and f_idx > 0 and row_strs[0]:
                    ctx_parts.append(f"head={row_strs[0]!r}")
                slots.append(
                    {
                        "id": slot_id,
                        "file": entry.name,
                        "row_index": r_idx,
                        "field_index": f_idx,
                        "field_name": spec.field_name,
                        "source": src_text,
                        "max_bytes": spec.max_bytes,
                        "max_width_px": spec.max_width_px,
                        "batch": spec.batch,
                        "tokens": extract_tokens(src_text),
                        "context": " | ".join(ctx_parts),
                    }
                )
                file_counts[entry.name] += 1
                batch_slot_counts[spec.batch] += 1

    # 3. Extract from all 258 .scn files (40 c/ep/* + 218 c/map/*)
    scn_files_count = 0
    scn_nonempty_files = 0
    for entry in vfs.entries:
        if not is_scn_file(entry.name):
            continue
        scn_files_count += 1
        scn = parse_scn_file(entry.name, entry.data)
        if scn.strings:
            scn_nonempty_files += 1
        for s_idx, src_text in enumerate(scn.strings):
            max_b, max_w, note = infer_scn_string_constraints(
                entry.name, s_idx, scn.title_str_idx, src_text
            )
            slot_id = f"{entry.name}#{s_idx}:str"
            prev_s = scn.strings[s_idx - 1] if s_idx > 0 else ""
            ctx = f"{entry.name} str[{s_idx}] ({note})"
            if prev_s:
                ctx += f" | prev={prev_s[:30]!r}"
            slots.append(
                {
                    "id": slot_id,
                    "file": entry.name,
                    "row_index": s_idx,
                    "field_index": 0,
                    "field_name": note,
                    "source": src_text,
                    "max_bytes": max_b,
                    "max_width_px": max_w,
                    "batch": 4,
                    "tokens": extract_tokens(src_text),
                    "context": ctx,
                }
            )
            file_counts[entry.name] += 1
            batch_slot_counts[4] += 1

    out_dir.mkdir(parents=True, exist_ok=True)

    # Build deduplicated unique string table keyed by source string.
    # For each unique source string, track the minimum (strictest) max_bytes across all slots
    # where it appears, as well as the list of slots and batches.
    unique_map: Dict[str, Dict[str, Any]] = {}
    existing_unique_path = out_dir / "unique_strings.json"
    existing_translations: Dict[str, Dict[str, Any]] = {}
    if preserve_existing and existing_unique_path.exists():
        try:
            loaded = json.loads(existing_unique_path.read_text(encoding="utf-8"))
            for item in loaded.get("strings", []):
                if item.get("translation"):
                    existing_translations[item["source"]] = item
        except Exception:
            pass

    for slot in slots:
        src = slot["source"]
        if not src.strip():
            continue
        if src not in unique_map:
            prev = existing_translations.get(src, {})
            unique_map[src] = {
                "source": src,
                "translation": prev.get("translation", ""),
                "status": prev.get("status", "untranslated"),
                "min_max_bytes": slot["max_bytes"],
                "min_max_width_px": slot["max_width_px"],
                "batches": [slot["batch"]],
                "occurrences": 1,
                "sample_id": slot["id"],
                "sample_context": slot["context"],
                "tokens": slot["tokens"],
                "has_hangul": bool(HANGUL_RE.search(src)),
                "notes": prev.get("notes", ""),
            }
        else:
            u = unique_map[src]
            u["occurrences"] += 1
            u["min_max_bytes"] = min(u["min_max_bytes"], slot["max_bytes"])
            if slot["max_width_px"] is not None:
                if u["min_max_width_px"] is None:
                    u["min_max_width_px"] = slot["max_width_px"]
                else:
                    u["min_max_width_px"] = min(u["min_max_width_px"], slot["max_width_px"])
            if slot["batch"] not in u["batches"]:
                u["batches"].append(slot["batch"])
                u["batches"].sort()

    summary = {
        "total_vfs_files_scanned": len(vfs.entries),
        "text_csv_files": csv_files_count,
        "scn_files_total": scn_files_count,
        "scn_files_with_strings": scn_nonempty_files,
        "total_slots": len(slots),
        "vfs_slots": len(slots) - file_counts.get("res/values/strings.xml", 0),
        "unique_nonempty_strings": len(unique_map),
        "unique_with_hangul": sum(1 for u in unique_map.values() if u["has_hangul"]),
        "batch_slot_counts": {str(k): v for k, v in sorted(batch_slot_counts.items())},
        "batch_unique_counts": {
            str(b): sum(1 for u in unique_map.values() if u["batches"][0] == b)
            for b in (1, 2, 3, 4)
        },
    }

    (out_dir / "catalog.json").write_text(
        json.dumps({"summary": summary, "slots": slots}, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )

    unique_list = list(unique_map.values())
    (out_dir / "unique_strings.json").write_text(
        json.dumps({"summary": summary, "strings": unique_list}, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )

    return summary


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Extract all player-facing strings from Heroes Lore 5")
    parser.add_argument("--vfs", default="apktool_modern/assets/data.vfs.mp3")
    parser.add_argument("--strings-xml", default="apktool_modern/res/values/strings.xml")
    parser.add_argument("-o", "--out-dir", default="translations")
    args = parser.parse_args(argv)

    summary = extract_all(Path(args.vfs), Path(args.strings_xml), Path(args.out_dir))
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
