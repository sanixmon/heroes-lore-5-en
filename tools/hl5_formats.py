#!/usr/bin/env python3
"""
Heroes Lore 5 (co.kr.eamobile.HeroesLore5) — Binary Resource Formats & BFont Layout Engine

Covers:
  1. All 57 text-bearing CSV .dat files in c/csv/*.dat and c/csv2/*.dat
  2. All 258 scenario/event script .scn files in c/ep/*/*.scn and c/map/*.scn
  3. Exact BFont width, line-count, and word-wrapping calculations matching libHeroesLore5.so:
       - BFont::IsEscape (0x43668)
       - BFont::GetWidth (0x436a0)
       - BFont::GetByteLenPrintable (0x43794)
       - BFont::GetLineCount (0x43880)
"""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

# BFont escape characters in libHeroesLore5.so at 0x157960 (11 color codes) + ';' (0x3b) line break.
# After applying the verified 1-byte patch at 0x157965 ('\'' 0x27 -> '~' 0x7e),
# '~' becomes the Yellow color escape code and '\'' is a normal printable ASCII character (6px).
ORIG_COLOR_ESCAPES: str = "&|${}'\"`^<["
PATCHED_COLOR_ESCAPES: str = "&|${}~\"`^<["
LINE_BREAK_CHAR: str = ";"

TOKEN_RE = re.compile(r"(#[0-9]{1,2}|%[0-9]*[ds]|EVENT_|^N[0-9])")
HANGUL_RE = re.compile(r"[\uac00-\ud7a3\u3131-\u318e]")


@dataclass(frozen=True)
class FieldSpec:
    field_index: int
    field_name: str
    len_bytes: int  # 1 (uint8) or 2 (uint16_le)
    max_bytes: int  # Exact safe C++ buffer limit (excluding NUL)
    max_width_px: Optional[int]  # Exact BFont pixel width for multiline wrapping, or None for single-line
    batch: int


@dataclass
class ParsedSlot:
    slot_id: str
    file: str
    row_index: int
    field_index: int
    field_name: str
    source: str
    max_bytes: int
    max_width_px: Optional[int]
    batch: int
    tokens: List[str] = field(default_factory=list)
    context_note: str = ""


def extract_tokens(text: str) -> List[str]:
    """Extract engine placeholders and color escape codes that should be tracked."""
    tokens = TOKEN_RE.findall(text)
    for ch in text:
        if ch in ORIG_COLOR_ESCAPES or ch == "~":
            tokens.append(ch)
    return tokens


def convert_source_yellow_escape(text: str) -> str:
    """Convert original Korean yellow color escape (') to patched (~)."""
    return text.replace("'", "~")


def get_csv_schema(vfs_path: str) -> Tuple[str, List[FieldSpec]]:
    """
    Return (schema_kind, [FieldSpec, ...]) for any of the 57 text-bearing CSV .dat files.
    All max_bytes limits and max_width_px values are verified from libHeroesLore5.so disassembly.
    """
    norm = vfs_path.lstrip("/\\")

    # 1. TextMgr single-string files (0x143430): [u8 L1][str1]
    if norm in (
        "c/csv/common_text.dat",
        "c/csv/ingame_text.dat",
        "c/csv/menu_text.dat",
        "c/csv/mission_text.dat",
        "c/csv/name.dat",
    ):
        # menu_text has multiline popups (156px in StatePopup::PopupSet); mission_text has achievement descriptions
        width = 156 if norm == "c/csv/menu_text.dat" else (130 if norm == "c/csv/mission_text.dat" else None)
        return "single_str", [FieldSpec(0, "text", 1, 254, width, 1)]

    # 2. Tips (EventProc::TipsLoad @ 0x6b428): [u8 L1][str1], drawn at 340px in StateGame::Paint (0xe1074)
    if norm == "c/csv/tips.dat":
        return "single_str", [FieldSpec(0, "tip", 1, 254, 340, 1)]

    # 3. Help files (TextMgr::LoadHelpString @ 0x143308): [u16 L1][title][u16 L2][body], drawn at 130px in CommonUi::DrawHelp (0x7ee54)
    if norm.startswith("c/csv2/help_"):
        return "help_pair", [
            FieldSpec(0, "title", 2, 254, None, 1),
            FieldSpec(1, "body", 2, 32766, 130, 1),
        ]

    # 4. Class names (HERO::LoadResClassInfo @ 0x8beb4): [u8 L1][name][59B], fixed char[20] -> max 19B
    if norm == "c/csv/class.dat":
        return "lead_str_tail", [FieldSpec(0, "class_name", 1, 19, None, 1)]

    # 5. Buildup / orb / stat modifiers (InitGlobalValue @ 0x9d510): [u8 L1][name][14B], fixed char[20] -> max 19B
    if norm == "c/csv/buildup.dat":
        return "lead_str_tail", [FieldSpec(0, "buildup_name", 1, 19, None, 2)]

    # 6. Set item options (ItemTable::LoadSetItemOptionTable @ 0xa3030): [u8 L1][name][8B], fixed char[21] -> max 20B
    if norm == "c/csv/set_option.dat":
        return "lead_str_tail", [FieldSpec(0, "set_name", 1, 20, None, 2)]

    # 7. Cash shop (CashShop::LoadCashShopData @ 0xde07c): [3B][u8 L1][name][2B][u8 L2][desc][tail], drawn at 160px (0xdc24c)
    if norm.startswith("c/csv/cash_"):
        return "cash_pair", [
            FieldSpec(0, "item_name", 1, 254, None, 2),
            FieldSpec(1, "description", 1, 254, 160, 2),
        ]

    # 8. Skills (HERO::LoadResSkillInfo @ 0x8bba4): [u8 L1][name][47B][u8 L2][desc], drawn at 130px (0x7b1dc)
    if norm.startswith("c/csv/skill_"):
        return "skill_pair", [
            FieldSpec(0, "skill_name", 1, 254, None, 2),
            FieldSpec(1, "description", 1, 254, 130, 2),
        ]

    # 9. Smithing (HERO::LoadMixSmithTableInfo @ 0x8b958): [2B][u8 L1][name][4B][u8 L2][recipe][13B]
    if norm.startswith("c/csv/smith_"):
        return "item_or_smith_pair", [
            FieldSpec(0, "item_name", 1, 20, None, 2),
            FieldSpec(1, "recipe_desc", 1, 255, 130, 2),
        ]

    # 10. Items (ItemTable::LoadItemTable @ 0xa38e0): [2B][u8 L1][name][4B][u8 L2][desc][tail], drawn at 130px (0x7b53c)
    if norm.startswith("c/csv/item_"):
        return "item_or_smith_pair", [
            FieldSpec(0, "item_name", 1, 20, None, 2),
            FieldSpec(1, "description", 1, 255, 130, 2),
        ]

    # 11. Mission titles (Mission::LoadMissionTable @ 0x8b73c): [u8 L1][title][34B], heap allocated
    if norm == "c/csv/mission_list.dat":
        return "lead_str_tail", [FieldSpec(0, "mission_title", 1, 254, None, 3)]

    # 12. Enemies (Monster::setEnemyData @ 0xc1a94): [u8 L1][name][128B], fixed char[20] -> max 19B
    if norm.startswith("c/csv/enemy_") and not norm.startswith("c/csv/enemy_g"):
        return "lead_str_tail", [FieldSpec(0, "enemy_name", 1, 19, None, 3)]

    # 13. Quests (QuestMgr::LoadQuestData @ 0xd40e8): [3B][u8 L1][title][u8 L2][desc][u8 L3][cat][tail], desc drawn at 140px (0xd3328)
    if norm.startswith("c/csv/quest_"):
        return "quest_triple", [
            FieldSpec(0, "quest_title", 1, 27, None, 3),
            FieldSpec(1, "description", 1, 199, 140, 3),
            FieldSpec(2, "category", 1, 27, None, 3),
        ]

    raise KeyError(f"Unsupported or non-text CSV file: {vfs_path!r}")


def is_text_csv_file(vfs_path: str) -> bool:
    try:
        get_csv_schema(vfs_path)
        return True
    except KeyError:
        return False


def is_scn_file(vfs_path: str) -> bool:
    norm = vfs_path.lstrip("/\\")
    return norm.startswith("c/ep/") or (norm.startswith("c/map/") and norm.endswith(".scn"))


def parse_csv_dat(vfs_path: str, data: bytes) -> List[List[str]]:
    """Parse a text-bearing CSV .dat file into a list of string lists per row."""
    kind, specs = get_csv_schema(vfs_path)
    row_count = struct.unpack_from("<H", data, 0)[0]
    pos = 2
    rows_out: List[List[str]] = []

    for r_idx in range(row_count):
        if pos + 2 > len(data):
            raise ValueError(f"{vfs_path}: truncated row header at row {r_idx}")
        rsize = struct.unpack_from("<H", data, pos)[0]
        row = data[pos + 2 : pos + 2 + rsize]
        if len(row) != rsize:
            raise ValueError(f"{vfs_path}: truncated row payload at row {r_idx}")
        pos += 2 + rsize

        if kind == "single_str":
            l1 = row[0]
            if 1 + l1 != len(row):
                raise ValueError(f"{vfs_path} row {r_idx}: single_str length mismatch")
            rows_out.append([row[1 : 1 + l1].decode("cp949")])

        elif kind == "lead_str_tail":
            l1 = row[0]
            rows_out.append([row[1 : 1 + l1].decode("cp949")])

        elif kind == "help_pair":
            l1 = struct.unpack_from("<H", row, 0)[0]
            s1 = row[2 : 2 + l1].decode("cp949")
            l2 = struct.unpack_from("<H", row, 2 + l1)[0]
            s2 = row[4 + l1 : 4 + l1 + l2].decode("cp949")
            if 4 + l1 + l2 != len(row):
                raise ValueError(f"{vfs_path} row {r_idx}: help_pair length mismatch")
            rows_out.append([s1, s2])

        elif kind == "cash_pair":
            l1 = row[3]
            s1 = row[4 : 4 + l1].decode("cp949")
            p2 = 4 + l1 + 2
            l2 = row[p2]
            s2 = row[p2 + 1 : p2 + 1 + l2].decode("cp949")
            rows_out.append([s1, s2])

        elif kind == "skill_pair":
            l1 = row[0]
            s1 = row[1 : 1 + l1].decode("cp949")
            p2 = 1 + l1 + 47
            l2 = row[p2]
            s2 = row[p2 + 1 : p2 + 1 + l2].decode("cp949")
            if p2 + 1 + l2 != len(row):
                raise ValueError(f"{vfs_path} row {r_idx}: skill_pair length mismatch")
            rows_out.append([s1, s2])

        elif kind == "item_or_smith_pair":
            l1 = row[2]
            s1 = row[3 : 3 + l1].decode("cp949")
            p2 = 3 + l1 + 4
            l2 = row[p2]
            s2 = row[p2 + 1 : p2 + 1 + l2].decode("cp949")
            rows_out.append([s1, s2])

        elif kind == "quest_triple":
            l1 = row[3]
            s1 = row[4 : 4 + l1].decode("cp949")
            p2 = 4 + l1
            l2 = row[p2]
            s2 = row[p2 + 1 : p2 + 1 + l2].decode("cp949")
            p3 = p2 + 1 + l2
            l3 = row[p3]
            s3 = row[p3 + 1 : p3 + 1 + l3].decode("cp949")
            rows_out.append([s1, s2, s3])

        else:
            raise ValueError(f"Unknown schema kind: {kind}")

    if pos != len(data):
        raise ValueError(f"{vfs_path}: trailing bytes ({len(data) - pos})")

    return rows_out


def compile_csv_dat(vfs_path: str, orig_data: bytes, new_rows: List[List[str]], encoding: str = "cp949") -> bytes:
    """
    Recompile a CSV .dat file by replacing string fields in each row while preserving all
    binary non-string columns and updating both string length fields and outer uint16_le row_size.
    """
    kind, specs = get_csv_schema(vfs_path)
    row_count = struct.unpack_from("<H", orig_data, 0)[0]
    if len(new_rows) != row_count:
        raise ValueError(f"{vfs_path}: expected {row_count} rows, got {len(new_rows)}")

    pos = 2
    out_chunks: List[bytes] = [struct.pack("<H", row_count)]

    for r_idx in range(row_count):
        rsize = struct.unpack_from("<H", orig_data, pos)[0]
        row = orig_data[pos + 2 : pos + 2 + rsize]
        pos += 2 + rsize
        strs = new_rows[r_idx]
        enc_strs = [s.encode(encoding) for s in strs]

        for f_idx, b_str in enumerate(enc_strs):
            spec = specs[f_idx]
            max_len = 255 if spec.len_bytes == 1 else 65535
            if len(b_str) > max_len:
                raise ValueError(
                    f"{vfs_path} row {r_idx} field {spec.field_name}: encoded length {len(b_str)} "
                    f"exceeds integer type limit {max_len}"
                )

        if kind == "single_str":
            b1 = enc_strs[0]
            new_row = bytes([len(b1)]) + b1

        elif kind == "lead_str_tail":
            orig_l1 = row[0]
            tail = row[1 + orig_l1 :]
            b1 = enc_strs[0]
            new_row = bytes([len(b1)]) + b1 + tail

        elif kind == "help_pair":
            b1, b2 = enc_strs
            new_row = struct.pack("<H", len(b1)) + b1 + struct.pack("<H", len(b2)) + b2

        elif kind == "cash_pair":
            orig_l1 = row[3]
            mid = row[4 + orig_l1 : 4 + orig_l1 + 2]
            p2 = 4 + orig_l1 + 2
            orig_l2 = row[p2]
            tail = row[p2 + 1 + orig_l2 :]
            b1, b2 = enc_strs
            new_row = row[:3] + bytes([len(b1)]) + b1 + mid + bytes([len(b2)]) + b2 + tail

        elif kind == "skill_pair":
            orig_l1 = row[0]
            mid = row[1 + orig_l1 : 1 + orig_l1 + 47]
            b1, b2 = enc_strs
            new_row = bytes([len(b1)]) + b1 + mid + bytes([len(b2)]) + b2

        elif kind == "item_or_smith_pair":
            orig_l1 = row[2]
            mid = row[3 + orig_l1 : 3 + orig_l1 + 4]
            p2 = 3 + orig_l1 + 4
            orig_l2 = row[p2]
            tail = row[p2 + 1 + orig_l2 :]
            b1, b2 = enc_strs
            new_row = row[:2] + bytes([len(b1)]) + b1 + mid + bytes([len(b2)]) + b2 + tail

        elif kind == "quest_triple":
            orig_l1 = row[3]
            p2 = 4 + orig_l1
            orig_l2 = row[p2]
            p3 = p2 + 1 + orig_l2
            orig_l3 = row[p3]
            tail = row[p3 + 1 + orig_l3 :]
            b1, b2, b3 = enc_strs
            new_row = (
                row[:3]
                + bytes([len(b1)])
                + b1
                + bytes([len(b2)])
                + b2
                + bytes([len(b3)])
                + b3
                + tail
            )
        else:
            raise ValueError(f"Unknown schema kind: {kind}")

        if len(new_row) > 65535:
            raise ValueError(f"{vfs_path} row {r_idx}: row_size {len(new_row)} exceeds uint16")
        out_chunks.append(struct.pack("<H", len(new_row)))
        out_chunks.append(new_row)

    return b"".join(out_chunks)


@dataclass
class ScnFile:
    vfs_path: str
    is_mtf: bool  # True if b[0..3] == 03 03 03 <extra>
    t1: int
    t2: int
    t3: int
    extra_magic_byte: Optional[int]
    scene_header: bytes  # 12 bytes (map_id, title_str_idx, bgm_id, + 9 config bytes)
    scripts2_prefix: bytes  # raw scripts2 section bytes
    scripts1_prefix: bytes  # raw scripts1 section bytes
    str_dummy_byte: Optional[int]  # 1 byte if is_mtf or t3 == 3
    strings: List[str]

    @property
    def title_str_idx(self) -> int:
        return self.scene_header[1]


def _skip_scripts_section(data: bytes, pos: int, len_type: int) -> int:
    count = data[pos]
    cur = pos + 1
    total_bytecode = 0
    for _ in range(count):
        if len_type > 1:
            l = struct.unpack_from("<H", data, cur)[0]
            cur += 2
        else:
            l = data[cur]
            cur += 1
        total_bytecode += l
    cur += total_bytecode
    return cur


def parse_scn_file(vfs_path: str, data: bytes) -> ScnFile:
    """
    Parse a .scn file according to EventProc::Scene_Init (0x723a8) and Interpreter::open (0x9ed84).
    """
    b0, b1, b2 = data[0], data[1], data[2]
    if b0 == 3 and b1 == 3 and b2 == 3:
        is_mtf = True
        extra_magic_byte = data[3]
        pos = 4
        t1 = t2 = 2
        t3 = 3
    else:
        is_mtf = False
        extra_magic_byte = None
        pos = 3
        t1, t2, t3 = b0, b1, b2

    scene_header = data[pos : pos + 12]
    pos += 12

    s2_end = _skip_scripts_section(data, pos, t1)
    scripts2_prefix = data[pos:s2_end]
    pos = s2_end

    s1_end = _skip_scripts_section(data, pos, t2)
    scripts1_prefix = data[pos:s1_end]
    pos = s1_end

    if t3 == 3:
        str_dummy_byte = data[pos]
        str_count = struct.unpack_from("<H", data, pos + 1)[0]
        pos += 3
    else:
        str_dummy_byte = None
        str_count = data[pos]
        pos += 1

    lengths: List[int] = []
    for _ in range(str_count):
        if t3 > 1:
            l = struct.unpack_from("<H", data, pos)[0]
            pos += 2
        else:
            l = data[pos]
            pos += 1
        lengths.append(l)

    strings: List[str] = []
    for l in lengths:
        raw_s = data[pos : pos + l]
        pos += l
        strings.append(raw_s.decode("cp949"))

    if pos != len(data):
        raise ValueError(f"{vfs_path}: parsed {pos} bytes, expected {len(data)}")

    return ScnFile(
        vfs_path=vfs_path,
        is_mtf=is_mtf,
        t1=t1,
        t2=t2,
        t3=t3,
        extra_magic_byte=extra_magic_byte,
        scene_header=scene_header,
        scripts2_prefix=scripts2_prefix,
        scripts1_prefix=scripts1_prefix,
        str_dummy_byte=str_dummy_byte,
        strings=strings,
    )


def compile_scn_file(scn: ScnFile, new_strings: Optional[List[str]] = None, encoding: str = "cp949") -> bytes:
    """
    Recompile a ScnFile into binary bytes.
    If any string exceeds 255 bytes while scn.t3 <= 1 (and not is_mtf), automatically upgrades
    header byte t3 from 1 to 2 so Interpreter::Strings::init (0x9e7b0) reads uint16_le string lengths.
    """
    strs = new_strings if new_strings is not None else scn.strings
    if len(strs) != len(scn.strings):
        raise ValueError(f"{scn.vfs_path}: expected {len(scn.strings)} strings, got {len(strs)}")

    enc_strs = [s.encode(encoding) for s in strs]
    max_slen = max((len(b) for b in enc_strs), default=0)

    t3 = scn.t3
    if not scn.is_mtf and t3 <= 1 and max_slen > 255:
        t3 = 2  # Upgrade to 16-bit string lengths natively supported by Interpreter::Strings::init

    chunks: List[bytes] = []
    if scn.is_mtf:
        assert scn.extra_magic_byte is not None
        chunks.append(bytes([3, 3, 3, scn.extra_magic_byte]))
    else:
        chunks.append(bytes([scn.t1, scn.t2, t3]))

    chunks.append(scn.scene_header)
    chunks.append(scn.scripts2_prefix)
    chunks.append(scn.scripts1_prefix)

    if t3 == 3:
        dummy = scn.str_dummy_byte if scn.str_dummy_byte is not None else 0
        chunks.append(bytes([dummy]) + struct.pack("<H", len(enc_strs)))
    else:
        if len(enc_strs) > 255:
            raise ValueError(f"{scn.vfs_path}: string count {len(enc_strs)} exceeds uint8")
        chunks.append(bytes([len(enc_strs)]))

    for b_str in enc_strs:
        if t3 > 1:
            if len(b_str) > 65535:
                raise ValueError(f"{scn.vfs_path}: string length {len(b_str)} exceeds uint16")
            chunks.append(struct.pack("<H", len(b_str)))
        else:
            if len(b_str) > 255:
                raise ValueError(f"{scn.vfs_path}: string length {len(b_str)} exceeds uint8")
            chunks.append(bytes([len(b_str)]))

    chunks.extend(enc_strs)
    return b"".join(chunks)


# ==============================================================================
# Exact BFont Measurement & Line-Wrapping Engine (from libHeroesLore5.so)
# ==============================================================================

def is_bfont_escape(ch: int, patched: bool = True) -> bool:
    """
    Exact implementation of BFont::IsEscape(char) at 0x43668:
    Returns True if ch == ';' (0x3b) or ch is in the 11-byte color escape table.
    """
    if ch == 0x3B:  # ';'
        return True
    table = PATCHED_COLOR_ESCAPES.encode("ascii") if patched else ORIG_COLOR_ESCAPES.encode("ascii")
    return ch in table


def measure_bfont_width(raw_bytes: bytes, patched: bool = True, eng_width: int = 5, kor_width: int = 9) -> int:
    """
    Exact implementation of BFont::GetWidth(char*, int) at 0x436a0:
      - Leading spaces (' ' at char_count == 0) are skipped (0px).
      - ';' (0x3b) or 0x00 stops measurement and returns current width.
      - Color escape characters add 0px.
      - 2-byte CP949 characters (byte & 0x80 != 0) add (kor_width + 1) = 10px.
      - Printable 1-byte ASCII characters add (eng_width + 1) = 6px.
    """
    width = 0
    char_count = 0
    i = 0
    n = len(raw_bytes)
    while i < n:
        b = raw_bytes[i]
        if b == 0:
            break
        if b == 0x20 and char_count == 0:
            i += 1
            continue
        if b & 0x80:
            if i + 1 >= n or raw_bytes[i + 1] == 0:
                break
            width += kor_width + 1
            i += 2
            char_count += 1
        else:
            if b == 0x3B:  # ';' stops single-line GetWidth
                break
            if is_bfont_escape(b, patched=patched):
                i += 1
                continue
            width += eng_width + 1
            i += 1
            char_count += 1
    return width


def get_byte_len_printable(
    raw_bytes: bytes,
    start_idx: int,
    max_width_px: int,
    max_lines: int = 1,
    patched: bool = True,
    eng_width: int = 5,
    kor_width: int = 9,
) -> int:
    """
    Exact instruction-level port of BFont::GetByteLenPrintable(char*, int, int, int, int) at 0x43794.
    Returns the number of bytes consumed from start_idx for max_lines (default 1 line).
    """
    n = len(raw_bytes)
    idx = start_idx
    cur_w = 0
    line_cnt = 0

    while idx < n and line_cnt < max_lines:
        b = raw_bytes[idx]
        if b == 0:
            break
        if b == 0x20 and cur_w == 0:
            # 0x4381c: Leading space at start of line (cur_w == 0) is consumed without adding width
            idx += 1
            continue
        if b & 0x80:
            # 2-byte character (0x437dc)
            if cur_w + kor_width < max_width_px:
                cur_w += kor_width + 1
                idx += 2
            else:
                line_cnt += 1
                cur_w = 0
            continue
        if is_bfont_escape(b, patched=patched):
            # 0x43838: ';' increments line_cnt and resets cur_w = 0; color codes just advance idx
            if b == 0x3B:
                line_cnt += 1
                cur_w = 0
            idx += 1
            continue
        # Normal 1-byte ASCII character (0x43854)
        if max_width_px <= cur_w + eng_width:
            line_cnt += 1
            cur_w = 0
        else:
            cur_w += eng_width + 1
            idx += 1

    return idx - start_idx


def simulate_bfont_lines(
    text: str,
    max_width_px: int,
    patched: bool = True,
    encoding: str = "cp949",
) -> List[Tuple[bytes, int]]:
    """
    Split text into rendered lines using BFont::GetLineCount / BFont::DrawTextField's exact
    GetByteLenPrintable(max_lines=1) loop.
    Returns [(line_bytes, pixel_width), ...].
    """
    raw = text.encode(encoding)
    if not raw:
        return []
    lines: List[Tuple[bytes, int]] = []
    pos = 0
    n = len(raw)
    while pos < n:
        step = get_byte_len_printable(raw, pos, max_width_px, max_lines=1, patched=patched)
        if step <= 0:
            break
        seg = raw[pos : pos + step]
        # Strip trailing ';' when computing visual width of this line
        vis_seg = seg[:-1] if seg.endswith(b";") else seg
        lines.append((seg, measure_bfont_width(vis_seg, patched=patched)))
        pos += step
    return lines


def visible_char_width_px(word: str, patched: bool = True) -> int:
    """Return the rendered pixel width of a word (ignoring BFont color escape characters)."""
    raw = word.encode("cp949", errors="replace")
    return measure_bfont_width(raw, patched=patched)


def word_wrap_english(
    text: str,
    max_width_px: int,
    max_bytes: Optional[int] = None,
    patched: bool = True,
) -> str:
    """
    Word-wrap an English translation using ';' hard line breaks so that no word is split mid-word
    by BFont::GetByteLenPrintable's character-level wrap at max_width_px.

    Wait: notice a subtle detail in BFont::GetByteLenPrintable!
    When a line wraps naturally at a space boundary vs with an explicit ';', adding ';' replaces
    the space ' ' (1 byte for ' ' -> 1 byte for ';'), so byte length DOES NOT GROW when replacing
    a space with ';' at a line break!
    Preserves intentional paragraph/speaker breaks (';;' or explicit ';') when appropriate.
    """
    if not text or max_width_px is None or max_width_px <= 0:
        return text

    # Preserve leading map-banner code like 'N7' or 'EVENT_'
    paragraphs = text.split(";")
    wrapped_lines: List[str] = []

    for para in paragraphs:
        para_stripped = para.strip()
        if not para_stripped:
            wrapped_lines.append("")
            continue

        words = para_stripped.split(" ")
        cur_words: List[str] = []
        cur_w = 0

        for w in words:
            if not w:
                continue
            w_px = visible_char_width_px(w, patched=patched)
            if not cur_words:
                cur_words.append(w)
                cur_w = w_px
            else:
                # Space adds 6px; last char on line only needs cur_w + 6 + (w_px - 1) < max_width_px
                # which is equivalent to cur_w + 6 + w_px <= max_width_px
                if cur_w + 6 + w_px <= max_width_px:
                    cur_words.append(w)
                    cur_w += 6 + w_px
                else:
                    wrapped_lines.append(" ".join(cur_words))
                    cur_words = [w]
                    cur_w = w_px

        if cur_words:
            wrapped_lines.append(" ".join(cur_words))

    result = ";".join(wrapped_lines)
    if max_bytes is not None and len(result.encode("cp949", errors="replace")) > max_bytes:
        # If explicit ';' wrapping somehow pushed over max_bytes, return compact single-spaced form
        compact = " ".join(p.strip() for p in paragraphs if p.strip())
        if len(compact.encode("cp949", errors="replace")) <= max_bytes:
            return word_wrap_english(compact, max_width_px, max_bytes=None, patched=patched)
    return result
