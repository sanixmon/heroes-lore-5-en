#!/usr/bin/env python3
"""
Heroes Lore 5 — Phase 12 Offline Boot & Interactive Gameplay Integration Test
(tests/test_offline_gameplay.py)
"""
import os, sys, time, unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

import math, re, struct, unicorn
from unicorn.arm_const import *
from poc.arm32_execution_engine import Arm32ExecutionEngine
from poc.jni_bridge import JniBridge, HandleType

engine = Arm32ExecutionEngine('apktool_modern/lib/armeabi/libHeroesLore5.so', base_addr=0x40000000)
bridge = JniBridge(engine)

table_vaddr = struct.unpack('<I', engine.uc.mem_read(bridge.jnienv32_ptr, 4))[0]
vm_table_vaddr = struct.unpack('<I', engine.uc.mem_read(bridge.javavm32_ptr, 4))[0]

def jni_vm_GetEnv(args):
    if args[1] != 0:
        engine.uc.mem_write(args[1], struct.pack('<I', bridge.jnienv32_ptr))
    return 0

def unpack_va(args):
    va_ptr = args[3]
    if 0x10000 <= va_ptr <= 0x7FFFFFFF:
        return list(struct.unpack('<8I', engine.uc.mem_read(va_ptr, 32)))
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

bridge._register_jni_slot(vm_table_vaddr, 0x18, 'JavaVM::GetEnv', jni_vm_GetEnv)
bridge._register_jni_slot(table_vaddr, 0x1CC, 'JNIEnv::CallStaticObjectMethodV', call_static_obj_v)
bridge._register_jni_slot(table_vaddr, 0x208, 'JNIEnv::CallStaticIntMethodV', call_static_int_v)
bridge._register_jni_slot(table_vaddr, 0x214, 'JNIEnv::CallStaticLongMethodV', call_static_int_v)
bridge._register_jni_slot(table_vaddr, 0x238, 'JNIEnv::CallStaticVoidMethodV', call_static_void_v)
bridge._register_jni_slot(table_vaddr, 0x29C, 'JNIEnv::NewStringUTF_29C', bridge._jni_NewStringUTF)

name_to_id = {}
id_to_name = {}
def get_unique_num(va):
    name = bridge.handle_table.resolve(va[0])
    if name not in name_to_id:
        idx = len(name_to_id) + 1
        name_to_id[name] = idx
        id_to_name[idx] = name
    return name_to_id[name]

def get_unique_name(va):
    return id_to_name.get(va[0], '')

cls = 'co/kr/eamobile/CletEntry'
bridge.static_methods[(cls, 'getUniqueNumberFromStrings', '(Ljava/lang/String;)I')] = get_unique_num
bridge.static_methods[(cls, 'getUniqueAssetNameFromID', '(I)Ljava/lang/String;')] = get_unique_name
bridge.static_methods[(cls, 'getMin', '()Ljava/lang/String;')] = lambda va: '01000000000'
bridge.static_methods[(cls, 'freeMemory', '()I')] = lambda va: 64 * 1024 * 1024
bridge.static_methods[(cls, 'totalMemory', '()I')] = lambda va: 128 * 1024 * 1024
bridge.static_methods[(cls, 'isNetworkAvailable', '()I')] = lambda va: 0

def hook_pow(args):
    x_bits = (args[1] << 32) | args[0]
    y_bits = (args[3] << 32) | args[2]
    x = struct.unpack('<d', struct.pack('<Q', x_bits))[0]
    y = struct.unpack('<d', struct.pack('<Q', y_bits))[0]
    res = math.pow(x, y)
    res_bits = struct.unpack('<Q', struct.pack('<d', res))[0]
    engine.uc.reg_write(UC_ARM_REG_R1, (res_bits >> 32) & 0xFFFFFFFF)
    return res_bits & 0xFFFFFFFF

sim_time_ms = [10000]
def hook_ftime(args):
    tp_ptr = args[0]
    sim_time_ms[0] += 50
    sec = sim_time_ms[0] // 1000
    ms = sim_time_ms[0] % 1000
    engine.uc.mem_write(tp_ptr, struct.pack('<IHhh', sec, ms, 0, 0))
    return 0

def hook_vsnprintf(args):
    buf_ptr, size, fmt_ptr, va_ptr = args[0], args[1], args[2], args[3]
    fmt = bridge.read_guest_string(fmt_ptr) if fmt_ptr >= 0x10000 else ''
    va_idx = [0]
    def repl(m):
        spec = m.group(0)
        if spec == '%%':
            return '%'
        val = struct.unpack('<I', engine.uc.mem_read(va_ptr + va_idx[0]*4, 4))[0]
        va_idx[0] += 1
        if spec.endswith('s'):
            return bridge.read_guest_string(val) if val >= 0x10000 else ''
        elif spec.endswith(('d', 'i')):
            if val & 0x80000000:
                val -= 0x100000000
            return (spec % val)
        elif spec.endswith(('u', 'x', 'X')):
            return (spec % val)
        elif spec.endswith('c'):
            return chr(val & 0xFF)
        return str(val)
    out = re.sub(r'%(?:%|[-+0 #]*\d*(?:\.\d+)?[hlL]?[sdiuxXc])', repl, fmt)
    raw = out.encode('utf-8')[:max(0, size - 1)] + b'\x00'
    engine.uc.mem_write(buf_ptr, raw)
    return len(raw) - 1

def hook_strcpy(args):
    s = bridge.read_guest_string(args[1]).encode('utf-8') + b'\x00'
    engine.uc.mem_write(args[0], s)
    return args[0]

def hook_strncpy(args):
    n = args[2]
    s = bridge.read_guest_string(args[1]).encode('utf-8')[:n]
    s = s.ljust(n, b'\x00')
    engine.uc.mem_write(args[0], s)
    return args[0]

def hook_strcat(args):
    s1 = bridge.read_guest_string(args[0])
    s2 = bridge.read_guest_string(args[1])
    raw = (s1 + s2).encode('utf-8') + b'\x00'
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

engine.register_host_hook('pow', hook_pow)
engine.register_host_hook('ftime', hook_ftime)
engine.register_host_hook('vsnprintf', hook_vsnprintf)
engine.register_host_hook('strcpy', hook_strcpy)
engine.register_host_hook('strncpy', hook_strncpy)
engine.register_host_hook('strcat', hook_strcat)
engine.register_host_hook('strcmp', hook_strcmp)
engine.register_host_hook('strncmp', hook_strncmp)
engine.register_host_hook('connect', lambda args: 0xFFFFFFFF)
engine.register_host_hook('socket', lambda args: 0xFFFFFFFF)

bridge.invoke_nativeInitKernel()
thiz = bridge.handle_table.allocate(HandleType.LOCAL_REF, 'Clet')
mag_bits = struct.unpack('<I', struct.pack('<f', 2.0))[0]
engine.call_function(
    'Java_co_kr_eamobile_CletEntry_nativeInitDisplay',
    bridge.jnienv32_ptr, thiz, 800, 480,
    5, 6, 5, 0,
    400, 240, 0, 0,
    0, mag_bits, 0, 0,
    0, 0
)
bridge.invoke_nativeInitFileStorage('/tmp/hl5_int', '/tmp/hl5_ext')
vfs_fd = bridge.vfs_adapter.register_asset('apktool_modern/assets/data.vfs.mp3', 0, 16452042)
bridge.invoke_nativeInitVFS(vfs_fd, 0, 16452042)
bridge.invoke_nativeInitAudioConfiguration(202400)
engine.call_function('Java_co_kr_eamobile_CletEntry_nativeStartApp', bridge.jnienv32_ptr, thiz, timeout_us=10000000)


game_obj = struct.unpack('<I', engine.uc.mem_read(0x40164368, 4))[0]
touch_sym = 'Java_co_kr_eamobile_CletEntry_nativeTouchHandleEvent'
loop_sym = 'Java_co_kr_eamobile_CletEntry_nativeLoop'

class TestOfflineGameplay(unittest.TestCase):
    def test_01_full_offline_boot_and_main_menu_gameplay(self):
        """Execute full offline state progression: State 0 -> State 1 -> State 2 -> State 3 -> State 4."""
        for f in range(1, 275):
            if f == 18 or f == 37 or (f > 120 and f < 130 and f % 5 == 0) or f == 263:
                sim_time_ms[0] += 65000
            if f == 19 or f == 38 or f == 127 or f == 264:
                engine.call_function(touch_sym, bridge.jnienv32_ptr, thiz, 0x13, 200, 140, 0)
                engine.call_function(touch_sym, bridge.jnienv32_ptr, thiz, 0x14, 200, 140, 0)

            current_state_ptr = struct.unpack('<I', engine.uc.mem_read(game_obj + 0x27c, 4))[0]
            vtable = struct.unpack('<I', engine.uc.mem_read(current_state_ptr, 4))[0] - 0x40000000

            if vtable == 0x163ae0: # StateMainMenu
                state = struct.unpack('<b', engine.uc.mem_read(current_state_ptr + 0xc, 1))[0]
                substate = struct.unpack('<b', engine.uc.mem_read(current_state_ptr + 0xd, 1))[0]
                if state == 2 and substate == 1:
                    engine.call_function(touch_sym, bridge.jnienv32_ptr, thiz, 0x13, 260, 160, 0)
                    engine.call_function(touch_sym, bridge.jnienv32_ptr, thiz, 0x14, 260, 160, 0)
                elif state == 2 and substate == 4:
                    engine.call_function(touch_sym, bridge.jnienv32_ptr, thiz, 0x13, 200, 120, 0)
                    engine.call_function(touch_sym, bridge.jnienv32_ptr, thiz, 0x14, 200, 120, 0)
                elif state == 3 and substate == 1 and f == 260:
                    engine.call_function(touch_sym, bridge.jnienv32_ptr, thiz, 0x13, 200, 120, 0)
                    engine.call_function(touch_sym, bridge.jnienv32_ptr, thiz, 0x14, 200, 120, 0)

            engine.call_function(loop_sym, bridge.jnienv32_ptr, thiz, timeout_us=10000000)

        # Verify reached State 4 (Main Menu)
        current_state_ptr = struct.unpack('<I', engine.uc.mem_read(game_obj + 0x27c, 4))[0]
        vtable_final = struct.unpack('<I', engine.uc.mem_read(current_state_ptr, 4))[0] - 0x40000000
        state_final = struct.unpack('<b', engine.uc.mem_read(current_state_ptr + 0xc, 1))[0]
        self.assertEqual(vtable_final, 0x163ae0, "Final state must be StateMainMenu")
        self.assertEqual(state_final, 4, "Must successfully reach State 4 (Main Menu)")

        # Verify framebuffer pixel rendering
        graphic_obj = struct.unpack('<I', engine.uc.mem_read(0x4016fcc8, 4))[0]
        fb_ptr = struct.unpack('<I', engine.uc.mem_read(graphic_obj, 4))[0]
        fb_bytes = engine.uc.mem_read(fb_ptr, 400 * 240 * 2)
        pixels = struct.unpack('<96000H', fb_bytes)
        unique_colors = len(set(pixels))
        print(f"\n[PASS] Reached Main Menu! FrameBuffer=0x{fb_ptr:08x}, Unique Colors={unique_colors}")
        self.assertGreater(unique_colors, 200, "Main Menu must have non-empty graphical rendering (>200 unique colors)")

if __name__ == '__main__':
    unittest.main()
