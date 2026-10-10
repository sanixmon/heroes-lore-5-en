#!/usr/bin/env python3
"""
Heroes Lore 5 — Virtual File System (VFS) Host-to-Guest Adapter
(poc/vfs_adapter.py)

Arsitektur:
  - Approach B: Guest-visible virtual file descriptor with typed host adapter.
  - Zero 64-bit pointer truncation: File streams (FILE*) dan file descriptors
    dikelola dalam tabel handle 32-bit terisolasi (guest_fd: 100.., guest_fp: 200..).
  - Isolasi Memori Guest: Seluruh operasi fread memverifikasi batas alamat virtual
    guest (< 4GB) sebelum menulis data.
  - Penegakan Jendela VFS: Operasi fseek dan fread dibatasi secara ketat dalam
    rentang [vfs_start_offset, vfs_start_offset + vfs_length].
"""

from __future__ import annotations

import io
import os
import struct
from pathlib import Path
from typing import Any, Dict, Optional, Tuple


class VfsStream:
    def __init__(
        self,
        filepath: str | Path,
        start_offset: int = 0,
        length: Optional[int] = None,
        is_dup: bool = False,
    ) -> None:
        self.filepath = Path(filepath)
        if not self.filepath.exists():
            raise FileNotFoundError(f"VFS asset not found: {self.filepath}")

        self.start_offset = start_offset
        total_file_size = self.filepath.stat().st_size
        if length is None or length <= 0:
            self.length = total_file_size - start_offset
        else:
            self.length = min(length, total_file_size - start_offset)

        # Buka stream file host independen
        self.file: io.BufferedReader = open(self.filepath, "rb")
        self.file.seek(self.start_offset, os.SEEK_SET)
        self.cursor = 0  # Posisi relatif terhadap start_offset
        self.is_dup = is_dup
        self.is_closed = False

    def dup(self) -> VfsStream:
        if self.is_closed:
            raise ValueError("Cannot duplicate closed VFS stream")
        new_stream = VfsStream(
            filepath=self.filepath,
            start_offset=self.start_offset,
            length=self.length,
            is_dup=True,
        )
        new_stream.seek(self.cursor, os.SEEK_SET)
        return new_stream

    def seek(self, offset: int, whence: int) -> int:
        if self.is_closed:
            raise ValueError("Cannot seek closed VFS stream")

        if whence == os.SEEK_SET:
            if self.start_offset > 0 and offset >= self.start_offset:
                target = offset - self.start_offset
            else:
                target = offset
        elif whence == os.SEEK_CUR:
            target = self.cursor + offset
        elif whence == os.SEEK_END:
            target = self.length + offset
        else:
            raise ValueError(f"Invalid seek whence: {whence}")

        # Batasan jendela VFS: 0 <= target <= length
        if target < 0:
            target = 0
        if target > self.length:
            target = self.length

        self.cursor = target
        self.file.seek(self.start_offset + self.cursor, os.SEEK_SET)
        return 0

    def tell(self) -> int:
        return self.cursor

    def read(self, size: int) -> bytes:
        if self.is_closed:
            raise ValueError("Cannot read from closed VFS stream")
        if size <= 0:
            return b""

        # Batasi pembacaan agar tidak keluar dari jendela VFS
        remaining = max(0, self.length - self.cursor)
        bytes_to_read = min(size, remaining)
        if bytes_to_read == 0:
            return b""

        data = self.file.read(bytes_to_read)
        self.cursor += len(data)
        return data

    def close(self) -> None:
        if not self.is_closed:
            self.file.close()
            self.is_closed = True


class VfsAdapter:
    """
    Adapter VFS yang menghubungkan panggilan libc guest (dup, fdopen, fseek, fread, fclose)
    ke stream aset game aktual di host.
    """

    def __init__(self) -> None:
        self._next_fd: int = 100
        self._next_fp: int = 200
        self._fd_table: Dict[int, VfsStream] = {}
        self._fp_table: Dict[int, VfsStream] = {}

    def register_asset(
        self,
        filepath: str | Path,
        start_offset: int = 0,
        length: Optional[int] = None,
    ) -> int:
        """Mendaftarkan aset game aktual dan mengembalikan descriptor 32-bit guest (guest_fd)."""
        stream = VfsStream(filepath, start_offset=start_offset, length=length)
        fd = self._next_fd
        self._next_fd += 1
        self._fd_table[fd] = stream
        return fd

    # -------------------------------------------------------------------------
    # libc Trampoline Callbacks (Dipanggil oleh ARM32 guest)
    # -------------------------------------------------------------------------

    def hook_dup(self, *args: Any) -> int:
        guest_fd = args[0][0] if isinstance(args[0], (list, tuple)) else args[0]
        if guest_fd not in self._fd_table:
            return -1  # EBADF
        try:
            cloned = self._fd_table[guest_fd].dup()
            new_fd = self._next_fd
            self._next_fd += 1
            self._fd_table[new_fd] = cloned
            return new_fd
        except Exception:
            return -1

    def hook_fdopen(self, *args: Any) -> int:
        if isinstance(args[0], (list, tuple)):
            guest_fd = args[0][0]
        else:
            guest_fd = args[0]
        if guest_fd not in self._fd_table:
            return 0  # NULL
        stream = self._fd_table[guest_fd]
        fp_handle = self._next_fp
        self._next_fp += 1
        self._fp_table[fp_handle] = stream
        return fp_handle

    def hook_fseek(self, *args: Any) -> int:
        if isinstance(args[0], (list, tuple)):
            fp_handle, offset, whence = args[0][0], args[0][1], args[0][2]
        else:
            fp_handle, offset, whence = args[0], args[1], args[2]
        if offset & 0x80000000:
            offset = -((~offset + 1) & 0xFFFFFFFF)
        if fp_handle not in self._fp_table:
            return -1
        try:
            res = self._fp_table[fp_handle].seek(offset, whence)
            return res
        except Exception:
            return -1

    def hook_fread(self, *args: Any, engine: Any = None) -> int:
        if isinstance(args[0], (list, tuple)):
            reg_args = args[0]
            guest_ptr, size, nmemb, fp_handle = reg_args[0], reg_args[1], reg_args[2], reg_args[3]
            eng = args[1] if len(args) > 1 else engine
        else:
            guest_ptr, size, nmemb, fp_handle = args[0], args[1], args[2], args[3]
            eng = args[4] if len(args) > 4 else engine

        total_bytes = size * nmemb
        if total_bytes == 0:
            return 0
        if fp_handle not in self._fp_table:
            return 0
        if guest_ptr < 0x10000 or (guest_ptr + total_bytes) > 0xFFFFFFFF:
            return 0
        stream = self._fp_table[fp_handle]
        try:
            data = stream.read(total_bytes)
            if not data:
                return 0
            if eng is not None and hasattr(eng, "uc"):
                eng.uc.mem_write(guest_ptr, data)
            items_read = len(data) // size if size > 0 else 0
            return items_read
        except Exception:
            return 0

    def hook_fclose(self, *args: Any) -> int:
        fp_handle = args[0][0] if isinstance(args[0], (list, tuple)) else args[0]
        if fp_handle in self._fp_table:
            stream = self._fp_table.pop(fp_handle)
            dup_fds = [k for k, v in self._fd_table.items() if v is stream and stream.is_dup]
            for dfd in dup_fds:
                self._fd_table.pop(dfd, None)
            stream.close()
            return 0
        return -1

    def close_all(self) -> None:
        for stream in list(self._fd_table.values()):
            stream.close()
        self._fd_table.clear()
        self._fp_table.clear()
