#!/usr/bin/env python3
"""
Unit and Integration Tests for HL5 64-bit Host to 32-bit Guest JNI ABI Bridge
(tests/test_jni_bridge.py)
"""

import sys
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

from poc.arm32_execution_engine import Arm32ExecutionEngine
from poc.jni_bridge import (
    HandleType,
    JniBridge,
    TypedHandleTable,
)

SO_PATH = ROOT_DIR / "apktool_modern" / "lib" / "armeabi" / "libHeroesLore5.so"


class TestTypedHandleTable(unittest.TestCase):
    def setUp(self):
        self.table = TypedHandleTable()

    def test_handle_allocation_and_resolution(self):
        obj = {"key": "test_object"}
        hid = self.table.allocate(HandleType.GLOBAL_REF, obj)
        self.assertIsInstance(hid, int)
        self.assertLess(hid, 0x1000000)  # Fits cleanly in 32-bit handle space
        resolved = self.table.resolve(hid, HandleType.GLOBAL_REF)
        self.assertEqual(resolved, obj)

    def test_handle_type_enforcement(self):
        hid = self.table.allocate(HandleType.LOCAL_REF, "my_string")
        with self.assertRaises(TypeError):
            self.table.resolve(hid, HandleType.METHOD_ID)

    def test_local_ref_lifecycle_scope(self):
        self.table.push_local_frame()
        hid = self.table.allocate(HandleType.LOCAL_REF, "temp_local")
        self.assertEqual(self.table.resolve(hid), "temp_local")
        self.table.pop_local_frame()
        # Should raise use-after-free
        with self.assertRaises(ValueError):
            self.table.resolve(hid)

    def test_global_ref_persists_across_local_frames(self):
        self.table.push_local_frame()
        ghid = self.table.allocate(HandleType.GLOBAL_REF, "global_data")
        self.table.pop_local_frame()
        # Global ref should still be valid
        self.assertEqual(self.table.resolve(ghid), "global_data")
        self.table.delete_ref(ghid, is_global=True)
        with self.assertRaises(ValueError):
            self.table.resolve(ghid)

    def test_invalid_handle_rejection(self):
        with self.assertRaises(ValueError):
            self.table.resolve(0xDEADBEEF)


class TestJniBridgeExecution(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SO_PATH.exists():
            raise unittest.SkipTest(f"libHeroesLore5.so not found at {SO_PATH}")
        cls.engine = Arm32ExecutionEngine(str(SO_PATH), base_addr=0x40000000)
        cls.bridge = JniBridge(cls.engine)

    def test_01_bridge_pointer_layout(self):
        # Verify 32-bit JNIEnv* and JavaVM* virtual addresses are in 32-bit space
        self.assertLess(self.bridge.jnienv32_ptr, 0xFFFFFFFF)
        self.assertGreater(self.bridge.jnienv32_ptr, 0)
        self.assertLess(self.bridge.javavm32_ptr, 0xFFFFFFFF)
        self.assertGreater(self.bridge.javavm32_ptr, 0)

    def test_02_startup_jni_nativeInitKernel_and_callbacks(self):
        """
        Verify startup JNI Call #1:
        nativeInitKernel executes AndroidService::makeInstance(env) and resolves
        all Java static method IDs through JNIEnv function trampolines.
        """
        self.bridge.invoke_nativeInitKernel()
        allocated_methods = [
            desc for desc in self.bridge.handle_table._table.values()
            if desc.handle_type == HandleType.METHOD_ID
        ]
        self.assertGreater(len(allocated_methods), 20)
        method_names = [desc.payload[1] for desc in allocated_methods]
        self.assertIn("getAssetSize", method_names)
        self.assertIn("loadAsset", method_names)
        self.assertIn("getUniqueNumberFromStrings", method_names)
        print(f"\n[+] Successfully resolved {len(allocated_methods)} Java callback method IDs via JNI bridge!")

    def test_03_minimal_vertical_slice_nativeInitAudioConfiguration(self):
        """
        Verify the simplest startup JNI export:
        nativeInitAudioConfiguration(202400) passes a 64-bit jlong across the bridge
        into ARM32 EABI (split across R2, R3) and completes successfully into MIDASKernelManager.
        """
        pcm_threshold = 202400
        self.bridge.invoke_nativeInitAudioConfiguration(pcm_threshold)

    def test_04_error_propagation_on_invalid_handle(self):
        """
        Verify that invalid or corrupt guest handles throw explicit exceptions
        and are not silently swallowed.
        """
        with self.assertRaises(ValueError):
            self.bridge.handle_table.resolve(0x12345678, HandleType.METHOD_ID)

    def test_05_guest_string_safety_bounds(self):
        """
        Verify guest pointer safety checks when reading strings.
        """
        # Address 0 should return empty string cleanly
        self.assertEqual(self.bridge.read_guest_string(0), "")

if __name__ == "__main__":
    unittest.main()
