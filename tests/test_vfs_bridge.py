#!/usr/bin/env python3
"""
Unit and Integration Tests for HL5 VFS Bridge & Native Engine Compatibility
(tests/test_vfs_bridge.py)

Covers all 10 required test cases:
  1. Valid asset open
  2. VFS header read matching source
  3. Cross-buffer reads
  4. Seek start, middle, and end
  5. EOF and out-of-bounds seek
  6. Asset not found
  7. Invalid or closed descriptor
  8. Non-zero start offset handling
  9. Resource release & lifecycle management
  10. No guest address boundary violations
"""

import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

from poc.arm32_execution_engine import Arm32ExecutionEngine
from poc.jni_bridge import JniBridge
from poc.vfs_adapter import VfsAdapter, VfsStream

SO_PATH = ROOT_DIR / "apktool_modern" / "lib" / "armeabi" / "libHeroesLore5.so"
VFS_PATH = ROOT_DIR / "apktool_modern" / "assets" / "data.vfs.mp3"


class TestVfsAdapterDirect(unittest.TestCase):
    """Direct unit tests for VfsStream and VfsAdapter operations."""

    def setUp(self):
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.dummy_data = b"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        self.dummy_file = Path(self.tmp_dir.name) / "test.dat"
        self.dummy_file.write_bytes(self.dummy_data)
        self.adapter = VfsAdapter()

    def tearDown(self):
        self.adapter.close_all()
        self.tmp_dir.cleanup()

    def test_01_valid_asset_open(self):
        """1. Aset valid berhasil dibuka."""
        fd = self.adapter.register_asset(self.dummy_file)
        self.assertGreaterEqual(fd, 100)
        self.assertIn(fd, self.adapter._fd_table)

    def test_02_vfs_header_matches_source(self):
        """2. Pembacaan header VFS sesuai dengan data sumber."""
        self.assertTrue(VFS_PATH.exists())
        # First 8 bytes of data.vfs.mp3 are hash and size of entry 0
        with open(VFS_PATH, "rb") as f:
            raw_h, raw_sz = struct.unpack("<II", f.read(8))

        fd = self.adapter.register_asset(VFS_PATH, start_offset=0, length=64)
        fp = self.adapter.hook_fdopen(fd, "rb")
        stream = self.adapter._fp_table[fp]
        header = stream.read(8)
        h, sz = struct.unpack("<II", header)
        self.assertEqual(h, raw_h)
        self.assertEqual(sz, raw_sz)
        self.adapter.hook_fclose(fp)

    def test_03_cross_buffer_reads(self):
        """3. Pembacaan lintas batas buffer."""
        stream = VfsStream(self.dummy_file)
        # Read in varying chunk sizes: 5, 7, 10, remaining
        part1 = stream.read(5)
        part2 = stream.read(7)
        part3 = stream.read(10)
        part4 = stream.read(len(self.dummy_data) - 22)
        combined = part1 + part2 + part3 + part4
        self.assertEqual(combined, self.dummy_data)
        stream.close()

    def test_04_seek_start_middle_end(self):
        """4. Seek ke awal, tengah, dan akhir berkas."""
        stream = VfsStream(self.dummy_file)
        # Seek start
        stream.seek(0, os.SEEK_SET)
        self.assertEqual(stream.tell(), 0)
        self.assertEqual(stream.read(4), b"0123")

        # Seek middle
        stream.seek(10, os.SEEK_SET)
        self.assertEqual(stream.tell(), 10)
        self.assertEqual(stream.read(4), b"ABCD")

        # Seek relative (CUR)
        stream.seek(2, os.SEEK_CUR)
        self.assertEqual(stream.tell(), 16)
        self.assertEqual(stream.read(2), b"GH")

        # Seek end
        stream.seek(-5, os.SEEK_END)
        self.assertEqual(stream.tell(), len(self.dummy_data) - 5)
        self.assertEqual(stream.read(5), self.dummy_data[-5:])
        stream.close()

    def test_05_eof_and_out_of_bounds_seek(self):
        """5. EOF dan seek di luar batas."""
        stream = VfsStream(self.dummy_file)
        # Seek past end: should clamp to EOF
        stream.seek(1000, os.SEEK_SET)
        self.assertEqual(stream.tell(), len(self.dummy_data))
        # Read at EOF must return empty bytes
        self.assertEqual(stream.read(10), b"")

        # Negative seek: should clamp to 0
        stream.seek(-50, os.SEEK_SET)
        self.assertEqual(stream.tell(), 0)
        self.assertEqual(stream.read(3), b"012")
        stream.close()

    def test_06_asset_not_found(self):
        """6. Aset tidak ditemukan."""
        missing = Path(self.tmp_dir.name) / "non_existent.dat"
        with self.assertRaises(FileNotFoundError):
            self.adapter.register_asset(missing)

    def test_07_invalid_or_closed_descriptor(self):
        """7. Descriptor tidak valid atau sudah ditutup."""
        # Non-existent descriptor
        self.assertEqual(self.adapter.hook_dup(9999), -1)
        self.assertEqual(self.adapter.hook_fdopen(9999, "rb"), 0)
        self.assertEqual(self.adapter.hook_fseek(9999, 0, os.SEEK_SET), -1)
        self.assertEqual(self.adapter.hook_fclose(9999), -1)

        # Closed descriptor
        fd = self.adapter.register_asset(self.dummy_file)
        fp = self.adapter.hook_fdopen(fd, "rb")
        self.adapter.hook_fclose(fp)
        # Attempting operation after fclose
        self.assertEqual(self.adapter.hook_fseek(fp, 0, os.SEEK_SET), -1)

    def test_08_non_zero_start_offset(self):
        """8. Penanganan offset aset yang bukan nol."""
        container_file = Path(self.tmp_dir.name) / "container.apk"
        prefix = b"APK_METADATA_HEADER_PADDING_" * 10  # 280 bytes
        container_file.write_bytes(prefix + self.dummy_data)

        start_off = len(prefix)
        stream = VfsStream(container_file, start_offset=start_off, length=len(self.dummy_data))
        # Cursor should start at 0 relative to start_offset
        self.assertEqual(stream.tell(), 0)
        self.assertEqual(stream.read(10), self.dummy_data[:10])

        # Absolute container seek (>= start_offset)
        stream.seek(start_off + 10, os.SEEK_SET)
        self.assertEqual(stream.tell(), 10)
        self.assertEqual(stream.read(4), b"ABCD")
        stream.close()

    def test_09_resource_lifecycle_deterministic_cleanup(self):
        """9. Pelepasan resource dan pengelolaan siklus hidup."""
        fd = self.adapter.register_asset(self.dummy_file)
        dup_fd = self.adapter.hook_dup(fd)
        fp = self.adapter.hook_fdopen(dup_fd, "rb")

        self.assertIn(fd, self.adapter._fd_table)
        self.assertIn(dup_fd, self.adapter._fd_table)
        self.assertIn(fp, self.adapter._fp_table)

        # Close duplicated stream
        self.assertEqual(self.adapter.hook_fclose(fp), 0)
        self.assertNotIn(fp, self.adapter._fp_table)
        self.assertNotIn(dup_fd, self.adapter._fd_table)

        # Original fd must remain valid
        self.assertIn(fd, self.adapter._fd_table)
        orig_fp = self.adapter.hook_fdopen(fd, "rb")
        self.assertGreater(orig_fp, 0)
        self.adapter.hook_fclose(orig_fp)


class TestVfsEngineIntegration(unittest.TestCase):
    """End-to-end integration tests with libHeroesLore5.so native engine."""

    @classmethod
    def setUpClass(cls):
        if not SO_PATH.exists() or not VFS_PATH.exists():
            raise unittest.SkipTest("Required binary or VFS assets missing")
        cls.engine = Arm32ExecutionEngine(str(SO_PATH), base_addr=0x40000000)
        cls.bridge = JniBridge(cls.engine)
        cls.bridge.invoke_nativeInitKernel()

        # Register actual game asset data.vfs.mp3
        cls.vfs_len = VFS_PATH.stat().st_size
        cls.guest_fd = cls.bridge.vfs_adapter.register_asset(VFS_PATH, 0, cls.vfs_len)
        cls.bridge.invoke_nativeInitVFS(cls.guest_fd, 0, cls.vfs_len)

    def test_02_vfs_header_matches_source_via_engine(self):
        """2. Pembacaan header VFS sesuai data sumber melalui fungsi engine nyata."""
        # Query version.txt via MIDASKernelManager::getAssetSizeFromVFS
        sz, off = self.bridge.invoke_getAssetSizeFromVFS("version.txt")
        self.assertEqual(sz, 7)
        self.assertEqual(off, 16452035)

        # Query c/csv/name.dat
        sz_name, off_name = self.bridge.invoke_getAssetSizeFromVFS("c/csv/name.dat")
        self.assertEqual(sz_name, 660)
        self.assertEqual(off_name, 314103)

        # Read actual data from source file and verify exact byte identity
        with open(VFS_PATH, "rb") as f:
            f.seek(off)
            version_bytes = f.read(sz)
            f.seek(off_name)
            name_bytes = f.read(sz_name)

        self.assertEqual(version_bytes, b"0.0.0\r\n")
        self.assertEqual(name_bytes[:16].hex(), "3f000500044e6f6e6508000753636875")

    def test_10_no_guest_address_boundary_violations(self):
        """10. Tidak ada pelanggaran batas alamat guest."""
        # Attempt to read into invalid address (e.g. 0x00000000 or 0xFFFFFF00)
        fd = self.bridge.vfs_adapter.register_asset(VFS_PATH, 0, 1024)
        fp = self.bridge.vfs_adapter.hook_fdopen(fd, "rb")

        # Reading into null or out of range memory must return 0 items
        items = self.bridge.vfs_adapter.hook_fread(
            [0x00000000, 1, 10, fp],
            self.engine,
        )
        self.assertEqual(items, 0)

        items_high = self.bridge.vfs_adapter.hook_fread(
            [0xFFFFFF00, 1, 100, fp],
            self.engine,
        )
        self.assertEqual(items_high, 0)
        self.bridge.vfs_adapter.hook_fclose(fp)


if __name__ == "__main__":
    unittest.main()
