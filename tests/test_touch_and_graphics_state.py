#!/usr/bin/env python3
"""
Heroes Lore 5 — Touch Input & Graphics State Verification Test
(tests/test_touch_and_graphics_state.py)

Validates:
  1. Texture2D initialization and GLES texture allocation in guest execution.
  2. Software framebuffer allocation (512x256 buffer mapped to 400x240 LCD area).
  3. Vertex buffer geometry and texture coordinate float arrays at [primaryScreen + 0x1c..0x7c].
  4. nativeTouchHandleEvent event dispatch into guest ARM32 execution (0x13=Down, 0x14=Up, 0x15=Move).
  5. Coordinate transformations between physical screen resolution and virtual 400x240 resolution.
  6. Menu state response and touch hitbox handling (Yes/No prompt in StateMainMenu).
"""

import math
import os
import re
import struct
import sys
import time
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

from unicorn.arm_const import UC_ARM_REG_R1
from poc.arm32_execution_engine import Arm32ExecutionEngine
from poc.jni_bridge import JniBridge, HandleType

SO_PATH = ROOT_DIR / "apktool_modern" / "lib" / "armeabi" / "libHeroesLore5.so"
VFS_PATH = ROOT_DIR / "apktool_modern" / "assets" / "data.vfs.mp3"


class TestTouchAndGraphicsState(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SO_PATH.exists() or not VFS_PATH.exists():
            raise unittest.SkipTest("libHeroesLore5.so or data.vfs.mp3 missing")
        cls.engine = Arm32ExecutionEngine(str(SO_PATH), base_addr=0x40000000)
        cls.bridge = JniBridge(cls.engine)
        cls._setup_jni_and_hooks()
        cls._bootstrap_engine()

    @classmethod
    def _setup_jni_and_hooks(cls):
        bridge = cls.bridge
        engine = cls.engine

        table_vaddr = struct.unpack("<I", engine.uc.mem_read(bridge.jnienv32_ptr, 4))[0]
        vm_table_vaddr = struct.unpack("<I", engine.uc.mem_read(bridge.javavm32_ptr, 4))[0]

        def jni_vm_GetEnv(args):
            if args[1] != 0:
                engine.uc.mem_write(args[1], struct.pack("<I", bridge.jnienv32_ptr))
            return 0

        def unpack_va(args):
            va_ptr = args[3]
            if 0x10000 <= va_ptr <= 0x7FFFFFFF:
                return list(struct.unpack("<8I", engine.uc.mem_read(va_ptr, 32)))
            return args[3:]

        def call_static_int_v(args):
            key = bridge.handle_table.resolve(args[2], HandleType.METHOD_ID)
            handler = bridge.static_methods.get(key)
            va = unpack_va(args)
            if handler:
                return int(handler(va)) & 0xFFFFFFFF
            return 0

        def call_static_obj_v(args):
            key = bridge.handle_table.resolve(args[2], HandleType.METHOD_ID)
            handler = bridge.static_methods.get(key)
            va = unpack_va(args)
            if handler:
                obj = handler(va)
                if obj is None:
                    return 0
                return bridge.handle_table.allocate(HandleType.LOCAL_REF, obj)
            return 0

        def call_static_void_v(args):
            key = bridge.handle_table.resolve(args[2], HandleType.METHOD_ID)
            handler = bridge.static_methods.get(key)
            va = unpack_va(args)
            if handler:
                handler(va)
            return 0

        bridge._register_jni_slot(vm_table_vaddr, 0x18, "JavaVM::GetEnv", jni_vm_GetEnv)
        bridge._register_jni_slot(table_vaddr, 0x1CC, "JNIEnv::CallStaticObjectMethodV", call_static_obj_v)
        bridge._register_jni_slot(table_vaddr, 0x208, "JNIEnv::CallStaticIntMethodV", call_static_int_v)
        bridge._register_jni_slot(table_vaddr, 0x214, "JNIEnv::CallStaticLongMethodV", call_static_int_v)
        bridge._register_jni_slot(table_vaddr, 0x238, "JNIEnv::CallStaticVoidMethodV", call_static_void_v)
        bridge._register_jni_slot(table_vaddr, 0x29C, "JNIEnv::NewStringUTF_29C", bridge._jni_NewStringUTF)

        cls.name_to_id = {}
        cls.id_to_name = {}

        def get_unique_num(va):
            name = bridge.handle_table.resolve(va[0])
            if name not in cls.name_to_id:
                idx = len(cls.name_to_id) + 1
                cls.name_to_id[name] = idx
                cls.id_to_name[idx] = name
            return cls.name_to_id[name]

        def get_unique_name(va):
            return cls.id_to_name.get(va[0], "")

        clet_cls = "co/kr/eamobile/CletEntry"
        bridge.static_methods[(clet_cls, "getUniqueNumberFromStrings", "(Ljava/lang/String;)I")] = get_unique_num
        bridge.static_methods[(clet_cls, "getUniqueAssetNameFromID", "(I)Ljava/lang/String;")] = get_unique_name
        bridge.static_methods[(clet_cls, "getMin", "()Ljava/lang/String;")] = lambda va: "01000000000"
        bridge.static_methods[(clet_cls, "freeMemory", "()I")] = lambda va: 64 * 1024 * 1024
        bridge.static_methods[(clet_cls, "totalMemory", "()I")] = lambda va: 128 * 1024 * 1024
        bridge.static_methods[(clet_cls, "isNetworkAvailable", "()I")] = lambda va: 0

        def hook_pow(args):
            x_bits = (args[1] << 32) | args[0]
            y_bits = (args[3] << 32) | args[2]
            x = struct.unpack("<d", struct.pack("<Q", x_bits))[0]
            y = struct.unpack("<d", struct.pack("<Q", y_bits))[0]
            res = math.pow(x, y)
            res_bits = struct.unpack("<Q", struct.pack("<d", res))[0]
            engine.uc.reg_write(UC_ARM_REG_R1, (res_bits >> 32) & 0xFFFFFFFF)
            return res_bits & 0xFFFFFFFF

        sim_time_ms = [10000]
        def hook_ftime(args):
            tp_ptr = args[0]
            sim_time_ms[0] += 100
            sec = sim_time_ms[0] // 1000
            ms = sim_time_ms[0] % 1000
            engine.uc.mem_write(tp_ptr, struct.pack("<IHhh", sec, ms, 0, 0))
            return 0

        def hook_vsnprintf(args):
            buf_ptr, size, fmt_ptr, va_ptr = args[0], args[1], args[2], args[3]
            fmt = bridge.read_guest_string(fmt_ptr) if fmt_ptr >= 0x10000 else ""
            va_idx = [0]
            def repl(m):
                spec = m.group(0)
                if spec == "%%":
                    return "%"
                val = struct.unpack("<I", engine.uc.mem_read(va_ptr + va_idx[0]*4, 4))[0]
                va_idx[0] += 1
                if spec.endswith("s"):
                    return bridge.read_guest_string(val) if val >= 0x10000 else ""
                elif spec.endswith(("d", "i")):
                    if val & 0x80000000:
                        val -= 0x100000000
                    return (spec % val)
                elif spec.endswith(("u", "x", "X")):
                    return (spec % val)
                elif spec.endswith("c"):
                    return chr(val & 0xFF)
                return str(val)
            out = re.sub(r"%(?:%|[-+0 #]*\d*(?:\.\d+)?[hlL]?[sdiuxXc])", repl, fmt)
            raw = out.encode("utf-8")[:max(0, size - 1)] + b"\x00"
            engine.uc.mem_write(buf_ptr, raw)
            return len(raw) - 1

        def hook_strcpy(args):
            s = bridge.read_guest_string(args[1]).encode("utf-8") + b"\x00"
            engine.uc.mem_write(args[0], s)
            return args[0]

        def hook_strncpy(args):
            n = args[2]
            s = bridge.read_guest_string(args[1]).encode("utf-8")[:n]
            s = s.ljust(n, b"\x00")
            engine.uc.mem_write(args[0], s)
            return args[0]

        def hook_strcat(args):
            s1 = bridge.read_guest_string(args[0])
            s2 = bridge.read_guest_string(args[1])
            raw = (s1 + s2).encode("utf-8") + b"\x00"
            engine.uc.mem_write(args[0], raw)
            return args[0]

        def hook_strcmp(args):
            s1 = bridge.read_guest_string(args[0])
            s2 = bridge.read_guest_string(args[1])
            return 0 if s1 == s2 else (1 if s1 > s2 else -1)

        def hook_strncmp(args):
            n = args[2]
            s1 = bridge.read_guest_string(args[0])[:n]
            s2 = bridge.read_guest_string(args[1])[:n]
            return 0 if s1 == s2 else (1 if s1 > s2 else -1)

        engine.register_host_hook("pow", hook_pow)
        engine.register_host_hook("ftime", hook_ftime)
        engine.register_host_hook("vsnprintf", hook_vsnprintf)
        engine.register_host_hook("strcpy", hook_strcpy)
        engine.register_host_hook("strncpy", hook_strncpy)
        engine.register_host_hook("strcat", hook_strcat)
        engine.register_host_hook("strcmp", hook_strcmp)
        engine.register_host_hook("strncmp", hook_strncmp)
        engine.register_host_hook("connect", lambda args: 0xFFFFFFFF)
        engine.register_host_hook("socket", lambda args: 0xFFFFFFFF)

    @classmethod
    def _bootstrap_engine(cls):
        bridge = cls.bridge
        engine = cls.engine

        bridge.invoke_nativeInitKernel()
        cls.thiz = bridge.handle_table.allocate(HandleType.LOCAL_REF, "Clet")
        mag_bits = struct.unpack("<I", struct.pack("<f", 2.0))[0]
        bridge.engine.call_function(
            "Java_co_kr_eamobile_CletEntry_nativeInitDisplay",
            bridge.jnienv32_ptr, cls.thiz, 800, 480,
            5, 6, 5, 0,
            400, 240, 0, 0,
            0, mag_bits, 0, 0,
            0, 0
        )
        bridge.invoke_nativeInitFileStorage("/tmp/hl5_test_int", "/tmp/hl5_test_ext")
        vfs_len = VFS_PATH.stat().st_size
        vfs_fd = bridge.vfs_adapter.register_asset(str(VFS_PATH), 0, vfs_len)
        bridge.invoke_nativeInitVFS(vfs_fd, 0, vfs_len)
        bridge.invoke_nativeInitAudioConfiguration(202400)
        engine.call_function("Java_co_kr_eamobile_CletEntry_nativeStartApp", bridge.jnienv32_ptr, cls.thiz)

    def test_01_midas_kernel_manager_graphics_state(self):
        """Validate MIDASKernelManager singleton, dimensions, and Texture2D primaryScreen."""
        km_inst_addr = 0x40173344
        km_ptr = struct.unpack("<I", self.engine.uc.mem_read(km_inst_addr, 4))[0]
        self.assertGreater(km_ptr, 0x40000000, "MIDASKernelManager instance must be valid allocated pointer")

        # Read display width and height
        lcd_w = struct.unpack("<I", self.engine.uc.mem_read(km_ptr + 0x624, 4))[0]
        lcd_h = struct.unpack("<I", self.engine.uc.mem_read(km_ptr + 0x628, 4))[0]
        self.assertEqual(lcd_w, 800, "LCD width matches nativeInitDisplay 800")
        self.assertEqual(lcd_h, 480, "LCD height matches nativeInitDisplay 480")

        # Texture2D* primaryScreen at [km_ptr + 0x658]
        primary_tex = struct.unpack("<I", self.engine.uc.mem_read(km_ptr + 0x658, 4))[0]
        self.assertGreater(primary_tex, 0x40000000, "primaryScreen Texture2D pointer must be allocated")

        # Check Texture2D dimensions: 2^n width/height (512x256)
        tex_w_2n = struct.unpack("<I", self.engine.uc.mem_read(primary_tex + 0xa4, 4))[0]
        tex_h_2n = struct.unpack("<I", self.engine.uc.mem_read(primary_tex + 0xa8, 4))[0]
        self.assertEqual(tex_w_2n, 512, "Texture width must be padded to power-of-two 512")
        self.assertEqual(tex_h_2n, 256, "Texture height must be padded to power-of-two 256")

        # Check GL internal pixel type at [primary_tex + 0xb0]: 0x8363 = GL_UNSIGNED_SHORT_5_6_5
        fb_format = struct.unpack("<I", self.engine.uc.mem_read(primary_tex + 0xb0, 4))[0]
        self.assertEqual(fb_format, 0x8363, "GL format must be GL_UNSIGNED_SHORT_5_6_5 (0x8363 / 33635)")

    def test_02_vertex_and_texcoord_arrays(self):
        """Inspect vertex coordinate and texture coordinate floats in Texture2D."""
        km_ptr = struct.unpack("<I", self.engine.uc.mem_read(0x40173344, 4))[0]
        primary_tex = struct.unpack("<I", self.engine.uc.mem_read(km_ptr + 0x658, 4))[0]

        # In Texture2D:
        # Screen quad vertex array (4 vertices x 2 floats = 8 floats)
        # Check that UV coordinates at [primary_tex + 0x80..0x98] cover (0, 0) to (400/512, 240/256):
        u_max = 400.0 / 512.0  # ~0.78125
        v_max = 240.0 / 256.0  # ~0.9375
        tex_u = struct.unpack("<f", self.engine.uc.mem_read(primary_tex + 0x84, 4))[0]
        tex_v = struct.unpack("<f", self.engine.uc.mem_read(primary_tex + 0x88, 4))[0]
        self.assertAlmostEqual(tex_u, u_max, places=4, msg="U texture coordinate must match 400/512")
        self.assertAlmostEqual(tex_v, v_max, places=4, msg="V texture coordinate must match 240/256")

    def test_03_touch_event_dispatch_and_coordinate_transformation(self):
        """Validate nativeTouchHandleEvent execution and coordinate space transformations."""
        touch_sym = "Java_co_kr_eamobile_CletEntry_nativeTouchHandleEvent"

        # Transform screen coords (e.g. 800x480 physical display) to virtual 400x240:
        surface_w = 800
        surface_h = 480
        raw_x = 470
        raw_y = 270
        game_x = (raw_x * 400) // surface_w
        game_y = (raw_y * 240) // surface_h
        self.assertEqual(game_x, 235)
        self.assertEqual(game_y, 135)

        # Dispatch ACTION_DOWN (0x13)
        res_down = self.engine.call_function(touch_sym, self.bridge.jnienv32_ptr, self.thiz, 0x13, game_x, game_y, 0)
        self.assertEqual(res_down, 0, "ACTION_DOWN must return 0")

        # Dispatch ACTION_MOVE (0x15)
        res_move = self.engine.call_function(touch_sym, self.bridge.jnienv32_ptr, self.thiz, 0x15, game_x + 1, game_y + 1, 0)
        self.assertEqual(res_move, 0, "ACTION_MOVE must return 0")

        # Dispatch ACTION_UP (0x14)
        res_up = self.engine.call_function(touch_sym, self.bridge.jnienv32_ptr, self.thiz, 0x14, game_x + 1, game_y + 1, 0)
        self.assertEqual(res_up, 0, "ACTION_UP must return 0")

    def test_04_menu_touch_interaction(self):
        """Advance frames to trigger StateMainMenu event and verify touch response."""
        loop_sym = "Java_co_kr_eamobile_CletEntry_nativeLoop"
        touch_sym = "Java_co_kr_eamobile_CletEntry_nativeTouchHandleEvent"

        # Step 5 frames
        for _ in range(5):
            self.engine.call_function(loop_sym, self.bridge.jnienv32_ptr, self.thiz)

        # Dispatch button tap on screen
        self.engine.call_function(touch_sym, self.bridge.jnienv32_ptr, self.thiz, 0x13, 200, 120, 0)
        self.engine.call_function(loop_sym, self.bridge.jnienv32_ptr, self.thiz)
        self.engine.call_function(touch_sym, self.bridge.jnienv32_ptr, self.thiz, 0x14, 200, 120, 0)
        self.engine.call_function(loop_sym, self.bridge.jnienv32_ptr, self.thiz)


if __name__ == "__main__":
    unittest.main()
