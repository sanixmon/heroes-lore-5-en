#!/usr/bin/env python3
"""
Heroes Lore 5 (co.kr.eamobile.HeroesLore5) — VFS Archive Tool (assets/data.vfs.mp3)

Binary format dissected from libHeroesLore5.so:
  - tempLocalGetExactFilename (0x387d0): strips leading '/' or '\\'
  - MIDASKernelManager::hash (0x1516a4): DJB2 hash (init=5381, h = ((h * 33) + byte) & 0xFFFFFFFF)
  - MIDASKernelManager::getAssetSizeFromVFS (0x151968):
      Sequential stream of [uint32_le djb2_hash][uint32_le size][uint8 payload[size]]
  - Entry [2187] (hash=0xbc909d54): Embedded manifest containing
      [uint32_le count=2188] followed by 2,188 NUL-terminated ASCII filenames
      (entries 0..2186 correspond to manifest[0..2186], entry 2187 is the manifest itself,
       and entry 2188 corresponds to manifest[2187] = 'version.txt').
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

MANIFEST_ENTRY_INDEX = 2187
MANIFEST_ENTRY_HASH = 0xBC909D54
MANIFEST_SYNTHETIC_NAME = "__manifest__.bin"


def normalize_vfs_path(path: str | bytes) -> bytes:
    """Strip leading '/' or '\\' just like tempLocalGetExactFilename (0x387d0)."""
    if isinstance(path, str):
        raw = path.encode("ascii")
    else:
        raw = path
    if raw.startswith((b"/", b"\\")):
        raw = raw[1:]
    return raw


def djb2_hash(path: str | bytes) -> int:
    """Compute 32-bit unsigned DJB2 hash matching MIDASKernelManager::hash (0x1516a4)."""
    norm = normalize_vfs_path(path)
    h = 5381
    for b in norm:
        h = ((h * 33) + b) & 0xFFFFFFFF
    return h


@dataclass
class VfsEntry:
    index: int
    name: str
    djb2: int
    size: int
    offset: int
    data: bytes

    @property
    def sha256(self) -> str:
        return hashlib.sha256(self.data).hexdigest()


class VfsArchive:
    def __init__(self, entries: List[VfsEntry], manifest_names: List[str]) -> None:
        self.entries = entries
        self.manifest_names = manifest_names
        self._by_name: Dict[str, VfsEntry] = {e.name: e for e in entries}

    @classmethod
    def from_bytes(cls, raw: bytes, verify: bool = True) -> "VfsArchive":
        raw_entries: List[Tuple[int, int, int, bytes]] = []
        pos = 0
        total_len = len(raw)
        while pos + 8 <= total_len:
            h, sz = struct.unpack_from("<II", raw, pos)
            pos += 8
            if pos + sz > total_len:
                raise ValueError(
                    f"Truncated VFS entry at index {len(raw_entries)} (offset 0x{pos - 8:x}): "
                    f"declared size {sz} exceeds remaining bytes {total_len - pos}"
                )
            payload = raw[pos : pos + sz]
            raw_entries.append((h, sz, pos, payload))
            pos += sz

        if pos != total_len:
            raise ValueError(f"Trailing bytes in VFS archive: parsed {pos} of {total_len} bytes")

        if len(raw_entries) != 2189:
            raise ValueError(f"Expected 2189 VFS entries, found {len(raw_entries)}")

        manifest_h, manifest_sz, manifest_off, manifest_payload = raw_entries[MANIFEST_ENTRY_INDEX]
        if manifest_h != MANIFEST_ENTRY_HASH:
            raise ValueError(
                f"Entry [{MANIFEST_ENTRY_INDEX}] hash mismatch: expected 0x{MANIFEST_ENTRY_HASH:08x}, "
                f"got 0x{manifest_h:08x}"
            )

        manifest_count = struct.unpack_from("<I", manifest_payload, 0)[0]
        parts = manifest_payload[4:].split(b"\x00")
        if len(parts) < manifest_count:
            raise ValueError(
                f"Manifest declares {manifest_count} filenames, but only {len(parts)} NUL-separated parts found"
            )
        manifest_names = [p.decode("ascii") for p in parts[:manifest_count]]

        if manifest_count != len(raw_entries) - 1:
            raise ValueError(
                f"Manifest filename count ({manifest_count}) != VFS entry count - 1 ({len(raw_entries) - 1})"
            )

        entries: List[VfsEntry] = []
        for idx, (h, sz, off, payload) in enumerate(raw_entries):
            if idx < MANIFEST_ENTRY_INDEX:
                name = manifest_names[idx]
                expected_h = djb2_hash(name)
            elif idx == MANIFEST_ENTRY_INDEX:
                name = MANIFEST_SYNTHETIC_NAME
                expected_h = MANIFEST_ENTRY_HASH
            else:
                name = manifest_names[idx - 1]
                expected_h = djb2_hash(name)

            if verify and h != expected_h:
                raise ValueError(
                    f"DJB2 hash mismatch at VFS entry [{idx}] ({name!r}): "
                    f"archive has 0x{h:08x}, computed 0x{expected_h:08x}"
                )
            entries.append(VfsEntry(index=idx, name=name, djb2=h, size=sz, offset=off, data=payload))

        return cls(entries=entries, manifest_names=manifest_names)

    @classmethod
    def from_file(cls, path: str | Path, verify: bool = True) -> "VfsArchive":
        return cls.from_bytes(Path(path).read_bytes(), verify=verify)

    def get(self, name: str) -> bytes:
        norm = normalize_vfs_path(name).decode("ascii")
        if norm not in self._by_name:
            raise KeyError(f"Asset not found in VFS: {norm!r}")
        return self._by_name[norm].data

    def set(self, name: str, data: bytes) -> None:
        norm = normalize_vfs_path(name).decode("ascii")
        if norm not in self._by_name:
            raise KeyError(f"Cannot set unknown asset in VFS: {norm!r}")
        entry = self._by_name[norm]
        entry.data = bytes(data)
        entry.size = len(entry.data)

    def build_manifest_bytes(self) -> bytes:
        body = b"\x00".join(n.encode("ascii") for n in self.manifest_names) + b"\x00"
        return struct.pack("<I", len(self.manifest_names)) + body

    def to_bytes(self, overrides: Optional[Dict[str, bytes]] = None) -> bytes:
        norm_overrides: Dict[str, bytes] = {}
        if overrides:
            for k, v in overrides.items():
                norm_overrides[normalize_vfs_path(k).decode("ascii")] = bytes(v)

        chunks: List[bytes] = []
        offset = 0
        for entry in self.entries:
            payload = norm_overrides.get(entry.name, entry.data)
            if entry.index == MANIFEST_ENTRY_INDEX:
                expected_manifest = self.build_manifest_bytes()
                if payload != expected_manifest:
                    raise ValueError("Manifest payload does not match manifest_names")
                expected_hash = MANIFEST_ENTRY_HASH
            else:
                expected_hash = djb2_hash(entry.name)

            if entry.djb2 != expected_hash:
                raise ValueError(
                    f"Entry [{entry.index}] ({entry.name}) hash 0x{entry.djb2:08x} != expected 0x{expected_hash:08x}"
                )
            header = struct.pack("<II", entry.djb2, len(payload))
            chunks.append(header)
            chunks.append(payload)
            offset += 8 + len(payload)

        return b"".join(chunks)

    def extract_to_dir(self, out_dir: str | Path) -> Path:
        out_path = Path(out_dir)
        out_path.mkdir(parents=True, exist_ok=True)
        index_meta = []
        for entry in self.entries:
            target = out_path / entry.name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(entry.data)
            index_meta.append(
                {
                    "index": entry.index,
                    "name": entry.name,
                    "djb2": f"0x{entry.djb2:08x}",
                    "size": entry.size,
                    "offset": entry.offset,
                    "sha256": entry.sha256,
                }
            )
        (out_path / "vfs_index.json").write_text(
            json.dumps(
                {
                    "entry_count": len(self.entries),
                    "manifest_count": len(self.manifest_names),
                    "entries": index_meta,
                },
                indent=2,
            ),
            encoding="utf-8",
        )
        return out_path

    @classmethod
    def repack_from_dir(cls, in_dir: str | Path) -> bytes:
        in_path = Path(in_dir)
        index_file = in_path / "vfs_index.json"
        if not index_file.exists():
            raise FileNotFoundError(f"Missing vfs_index.json in {in_path}")
        meta = json.loads(index_file.read_text(encoding="utf-8"))
        chunks: List[bytes] = []
        for item in meta["entries"]:
            name = item["name"]
            djb2_val = int(item["djb2"], 16)
            if name != MANIFEST_SYNTHETIC_NAME:
                computed = djb2_hash(name)
                if djb2_val != computed:
                    raise ValueError(f"Hash mismatch for {name}: {djb2_val:#x} != {computed:#x}")
            file_bytes = (in_path / name).read_bytes()
            chunks.append(struct.pack("<II", djb2_val, len(file_bytes)))
            chunks.append(file_bytes)
        return b"".join(chunks)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Heroes Lore 5 VFS Archive Tool (data.vfs.mp3)")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_verify = sub.add_parser("verify", help="Verify VFS structure, manifest, DJB2 hashes, and round-trip identity")
    p_verify.add_argument("vfs", nargs="?", default="apktool_modern/assets/data.vfs.mp3")

    p_unpack = sub.add_parser("unpack", help="Extract VFS contents to a directory")
    p_unpack.add_argument("vfs", default="apktool_modern/assets/data.vfs.mp3", nargs="?")
    p_unpack.add_argument("-o", "--out", default="extracted/vfs")

    p_repack = sub.add_parser("repack", help="Repack extracted directory into a VFS archive")
    p_repack.add_argument("in_dir", default="extracted/vfs", nargs="?")
    p_repack.add_argument("-o", "--out", required=True)

    args = parser.parse_args(argv)

    if args.cmd == "verify":
        vfs_path = Path(args.vfs)
        raw = vfs_path.read_bytes()
        orig_sha = hashlib.sha256(raw).hexdigest()
        archive = VfsArchive.from_bytes(raw, verify=True)
        repacked = archive.to_bytes()
        repack_sha = hashlib.sha256(repacked).hexdigest()
        if raw != repacked:
            print(f"FAIL: Round-trip mismatch! orig={orig_sha} repack={repack_sha}", file=sys.stderr)
            return 1
        print(
            f"OK: Verified {len(archive.entries)} VFS entries ({len(raw):,} bytes), "
            f"manifest={len(archive.manifest_names)} files, round-trip SHA256={orig_sha}"
        )
        return 0

    if args.cmd == "unpack":
        archive = VfsArchive.from_file(args.vfs, verify=True)
        out_path = archive.extract_to_dir(args.out)
        print(f"OK: Extracted {len(archive.entries)} entries to {out_path}")
        return 0

    if args.cmd == "repack":
        repacked = VfsArchive.repack_from_dir(args.in_dir)
        # Validate repacked bytes
        VfsArchive.from_bytes(repacked, verify=True)
        out_path = Path(args.out)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(repacked)
        print(f"OK: Repacked {len(repacked):,} bytes to {out_path} (SHA256={hashlib.sha256(repacked).hexdigest()})")
        return 0

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
