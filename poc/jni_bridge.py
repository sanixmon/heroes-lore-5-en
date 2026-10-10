#!/usr/bin/env python3
"""
Heroes Lore 5 — 64-bit Host to 32-bit Guest JNI ABI Bridge (poc/jni_bridge.py)

Architecture & Pointer Semantics:
  1. Host Reference / Object:
     A 64-bit native pointer or Python host object in the host runtime (sizeof(void*) = 8).
     NEVER passed directly into or truncated by the 32-bit guest.
  2. Guest Handle:
     An opaque 32-bit integer (1..0x00FFFFFF) managed by TypedHandleTable.
     The guest code treats this as a jobject, jclass, jstring, jmethodID, or jfieldID.
  3. Guest Virtual Address:
     A valid 32-bit pointer in the guest memory address space (< 4GB, mapped in Unicorn).
     Must be verified against mapped segment bounds before dereference.

Provides JNIEnv32 table layout matching Android ARM32 NDK:
  - offset 0x10 (16):  GetVersion() -> jint
  - offset 0x18 (24):  FindClass(const char* name) -> jclass (guest handle)
  - offset 0x54 (84):  NewGlobalRef(jobject obj) -> jobject (guest handle)
  - offset 0x58 (88):  DeleteGlobalRef(jobject obj) -> void
  - offset 0x5c (92):  DeleteLocalRef(jobject obj) -> void
  - offset 0x1c4 (452): GetStaticMethodID(jclass, const char* name, const char* sig) -> jmethodID
  - offset 0x1d4 (468): CallStaticObjectMethod(jclass, jmethodID, ...) -> jobject
  - offset 0x1ec (492): CallStaticIntMethod(jclass, jmethodID, ...) -> jint
  - offset 0x204 (516): CallStaticVoidMethod(jclass, jmethodID, ...) -> void
  - offset 0x294 (660): NewStringUTF(const char* bytes) -> jstring (guest handle)
  - offset 0x2a4 (676): GetStringUTFChars(jstring, jboolean* isCopy) -> const char* (guest ptr)
  - offset 0x2a8 (680): ReleaseStringUTFChars(jstring, const char* chars) -> void
  - offset 0x36c (876): GetJavaVM(JavaVM** vm) -> jint
"""

from __future__ import annotations

import enum
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Set, Tuple

import unicorn
from unicorn.arm_const import *

ROOT_DIR = Path(__file__).resolve().parent.parent
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

from poc.arm32_execution_engine import (
    EXIT_MAGIC_ADDR,
    PAGE_SIZE,
    STACK_BASE,
    STACK_SIZE,
    TRAMPOLINE_BASE,
    Arm32ExecutionEngine,
    align_up,
)
from poc.vfs_adapter import VfsAdapter



class HandleType(enum.Enum):
    LOCAL_REF = 1
    GLOBAL_REF = 2
    METHOD_ID = 3
    FIELD_ID = 4


@dataclass
class HostObjectDescriptor:
    handle_id: int
    handle_type: HandleType
    payload: Any
    class_name: str = ""
    signature: str = ""
    is_valid: bool = True


class TypedHandleTable:
    """
    Typed handle table storing 64-bit host objects and assigning 32-bit guest handles.
    Guarantees no 64-bit pointers are ever truncated into 32-bit registers.
    """

    def __init__(self) -> None:
        self._next_id: int = 100
        self._table: Dict[int, HostObjectDescriptor] = {}
        self._local_frames: List[Set[int]] = [set()]

    def push_local_frame(self) -> None:
        self._local_frames.append(set())

    def pop_local_frame(self) -> None:
        if len(self._local_frames) > 1:
            frame = self._local_frames.pop()
            for hid in frame:
                if hid in self._table and self._table[hid].handle_type == HandleType.LOCAL_REF:
                    self._table[hid].is_valid = False

    def allocate(
        self,
        handle_type: HandleType,
        payload: Any,
        class_name: str = "",
        signature: str = "",
    ) -> int:
        hid = self._next_id
        self._next_id += 1
        desc = HostObjectDescriptor(
            handle_id=hid,
            handle_type=handle_type,
            payload=payload,
            class_name=class_name,
            signature=signature,
            is_valid=True,
        )
        self._table[hid] = desc
        if handle_type == HandleType.LOCAL_REF:
            self._local_frames[-1].add(hid)
        return hid

    def resolve(self, handle_id: int, expected_type: Optional[HandleType] = None) -> Any:
        if handle_id not in self._table:
            raise ValueError(f"Invalid JNI handle: {handle_id:#x} does not exist in handle table")
        desc = self._table[handle_id]
        if not desc.is_valid:
            raise ValueError(f"Use-after-free: JNI handle {handle_id:#x} has been deleted or popped")
        if expected_type is not None and desc.handle_type != expected_type:
            raise TypeError(
                f"Handle type mismatch for {handle_id:#x}: expected {expected_type}, got {desc.handle_type}"
            )
        return desc.payload

    def delete_ref(self, handle_id: int, is_global: bool = False) -> None:
        if handle_id in self._table:
            desc = self._table[handle_id]
            expected = HandleType.GLOBAL_REF if is_global else HandleType.LOCAL_REF
            if desc.handle_type == expected:
                desc.is_valid = False


class JniBridge:
    """
    Cross-bitness JNI ABI Bridge between 64-bit Host and 32-bit ARM Guest Engine.
    """

    def __init__(self, engine: Arm32ExecutionEngine) -> None:
        self.engine = engine
        self.handle_table = TypedHandleTable()
        self.vfs_adapter = VfsAdapter()

        # Track registered Java callbacks on host
        self.static_methods: Dict[Tuple[str, str, str], Callable[..., Any]] = {}

        # 32-bit JNIEnv memory layout in guest space
        self.jnienv32_ptr = 0
        self.javavm32_ptr = 0
        self._setup_jnienv32()
        self._register_libc_filesystem_hooks()

    def _setup_jnienv32(self) -> None:
        # Allocate JNIEnv* struct and JNINativeInterface table in guest heap (< 4GB)
        # JNIEnv is a pointer to JNINativeInterface*
        table_size = 1024  # 256 function pointers * 4 bytes
        table_vaddr = self.engine.heap_curr
        self.engine.heap_curr += align_up(table_size, PAGE_SIZE)
        self.engine.uc.mem_write(table_vaddr, bytes(table_size))

        # JNIEnv struct: pointer to table_vaddr
        self.jnienv32_ptr = self.engine.heap_curr
        self.engine.heap_curr += 8
        self.engine.uc.mem_write(self.jnienv32_ptr, struct.pack("<I", table_vaddr))

        # JavaVM struct: pointer to JNIInvokeInterface table
        vm_table_vaddr = self.engine.heap_curr
        self.engine.heap_curr += align_up(128, PAGE_SIZE)
        self.engine.uc.mem_write(vm_table_vaddr, bytes(128))

        self.javavm32_ptr = self.engine.heap_curr
        self.engine.heap_curr += 8
        self.engine.uc.mem_write(self.javavm32_ptr, struct.pack("<I", vm_table_vaddr))

        # Register JNIEnv function trampolines at exact ARM32 byte offsets
        # JNI function pointer = table_vaddr + offset
        self._register_jni_slot(table_vaddr, 0x10, "JNIEnv::GetVersion", self._jni_GetVersion)
        self._register_jni_slot(table_vaddr, 0x18, "JNIEnv::FindClass", self._jni_FindClass)
        self._register_jni_slot(table_vaddr, 0x54, "JNIEnv::NewGlobalRef", self._jni_NewGlobalRef)
        self._register_jni_slot(table_vaddr, 0x58, "JNIEnv::DeleteGlobalRef", self._jni_DeleteGlobalRef)
        self._register_jni_slot(table_vaddr, 0x5C, "JNIEnv::DeleteLocalRef", self._jni_DeleteLocalRef)
        self._register_jni_slot(table_vaddr, 0x1C4, "JNIEnv::GetStaticMethodID", self._jni_GetStaticMethodID)
        self._register_jni_slot(table_vaddr, 0x1D4, "JNIEnv::CallStaticObjectMethod", self._jni_CallStaticObjectMethod)
        self._register_jni_slot(table_vaddr, 0x1EC, "JNIEnv::CallStaticIntMethod", self._jni_CallStaticIntMethod)
        self._register_jni_slot(table_vaddr, 0x204, "JNIEnv::CallStaticVoidMethod", self._jni_CallStaticVoidMethod)
        self._register_jni_slot(table_vaddr, 0x294, "JNIEnv::NewStringUTF", self._jni_NewStringUTF)
        self._register_jni_slot(table_vaddr, 0x2A4, "JNIEnv::GetStringUTFChars", self._jni_GetStringUTFChars)
        self._register_jni_slot(table_vaddr, 0x2A8, "JNIEnv::ReleaseStringUTFChars", self._jni_ReleaseStringUTFChars)
        self._register_jni_slot(table_vaddr, 0x36C, "JNIEnv::GetJavaVM", self._jni_GetJavaVM)
        self._register_jni_slot(table_vaddr, 0x178, "JNIEnv::GetFieldID", self._jni_GetFieldID)
        self._register_jni_slot(table_vaddr, 0x190, "JNIEnv::GetIntField", self._jni_GetIntField)

    def _register_jni_slot(
        self,
        table_vaddr: int,
        offset: int,
        name: str,
        handler: Callable[[List[int]], int],
    ) -> None:
        t_addr = self.engine.next_trampoline_addr
        self.engine.next_trampoline_addr += 8
        self.engine.trampoline_map[t_addr] = name
        self.engine.register_host_hook(name, handler)
        # Write 32-bit function pointer in the JNI table at offset
        self.engine.uc.mem_write(table_vaddr + offset, struct.pack("<I", t_addr))

    def read_guest_string(self, vaddr: int) -> str:
        if vaddr == 0:
            return ""
        chars = []
        cur = vaddr
        while True:
            b = self.engine.uc.mem_read(cur, 1)[0]
            if b == 0:
                break
            chars.append(b)
            cur += 1
            if len(chars) > 4096:
                raise ValueError("Guest string exceeded maximum safety length (4096 bytes)")
        return bytes(chars).decode("utf-8", errors="replace")
    def _register_libc_filesystem_hooks(self) -> None:
        self.engine.register_host_hook("dup", self.vfs_adapter.hook_dup)
        self.engine.register_host_hook("fdopen", self.vfs_adapter.hook_fdopen)
        self.engine.register_host_hook("fseek", self.vfs_adapter.hook_fseek)
        self.engine.register_host_hook("fread", lambda args: self.vfs_adapter.hook_fread(args, self.engine))
        self.engine.register_host_hook("fclose", self.vfs_adapter.hook_fclose)
        self.engine.register_host_hook("__errno", lambda args: self.engine.heap_curr)
        self.engine.register_host_hook("strdup", lambda args: self.engine.write_string(self.read_guest_string(args[0]), encoding="utf-8"))

    def _jni_GetFieldID(self, args: List[int]) -> int:
        cls_handle, name_ptr, sig_ptr = args[1], args[2], args[3]
        cls_name = self.handle_table.resolve(cls_handle, HandleType.LOCAL_REF)
        name = self.read_guest_string(name_ptr)
        sig = self.read_guest_string(sig_ptr)
        fid = self.handle_table.allocate(HandleType.FIELD_ID, (cls_name, name, sig))
        return fid

    def _jni_GetIntField(self, args: List[int]) -> int:
        obj_handle, fid_handle = args[1], args[2]
        obj = self.handle_table.resolve(obj_handle, HandleType.LOCAL_REF)
        if isinstance(obj, dict) and "descriptor" in obj:
            return int(obj["descriptor"])
        if isinstance(obj, int):
            return int(obj)
        return 0


    # --------------------------------------------------------------------------
    # JNI Function Trampoline Handlers (Executed when ARM32 engine calls JNIEnv)
    # --------------------------------------------------------------------------

    def _jni_GetVersion(self, args: List[int]) -> int:
        return 0x00010006  # JNI_VERSION_1_6

    def _jni_GetJavaVM(self, args: List[int]) -> int:
        # jint GetJavaVM(JNIEnv *env, JavaVM **vm);
        vm_out_ptr = args[1]
        if vm_out_ptr != 0:
            self.engine.uc.mem_write(vm_out_ptr, struct.pack("<I", self.javavm32_ptr))
        return 0  # JNI_OK

    def _jni_FindClass(self, args: List[int]) -> int:
        # jclass FindClass(JNIEnv *env, const char *name);
        name_ptr = args[1]
        cls_name = self.read_guest_string(name_ptr)
        hid = self.handle_table.allocate(HandleType.LOCAL_REF, cls_name, class_name=cls_name)
        return hid

    def _jni_NewGlobalRef(self, args: List[int]) -> int:
        obj_handle = args[1]
        if obj_handle == 0:
            return 0
        obj = self.handle_table.resolve(obj_handle)
        return self.handle_table.allocate(HandleType.GLOBAL_REF, obj)

    def _jni_DeleteGlobalRef(self, args: List[int]) -> int:
        obj_handle = args[1]
        self.handle_table.delete_ref(obj_handle, is_global=True)
        return 0

    def _jni_DeleteLocalRef(self, args: List[int]) -> int:
        obj_handle = args[1]
        self.handle_table.delete_ref(obj_handle, is_global=False)
        return 0

    def _jni_GetStaticMethodID(self, args: List[int]) -> int:
        # jmethodID GetStaticMethodID(JNIEnv *env, jclass clazz, const char *name, const char *sig);
        cls_handle, name_ptr, sig_ptr = args[1], args[2], args[3]
        cls_name = self.handle_table.resolve(cls_handle, HandleType.LOCAL_REF)
        name = self.read_guest_string(name_ptr)
        sig = self.read_guest_string(sig_ptr)
        key = (cls_name, name, sig)
        mid = self.handle_table.allocate(HandleType.METHOD_ID, key, class_name=cls_name, signature=sig)
        return mid

    def _jni_CallStaticVoidMethod(self, args: List[int]) -> int:
        cls_handle, mid_handle = args[1], args[2]
        key = self.handle_table.resolve(mid_handle, HandleType.METHOD_ID)
        handler = self.static_methods.get(key)
        if handler:
            handler(args[3:])
        return 0

    def _jni_CallStaticIntMethod(self, args: List[int]) -> int:
        cls_handle, mid_handle = args[1], args[2]
        key = self.handle_table.resolve(mid_handle, HandleType.METHOD_ID)
        handler = self.static_methods.get(key)
        if handler:
            return int(handler(args[3:])) & 0xFFFFFFFF
        return 0

    def _jni_CallStaticObjectMethod(self, args: List[int]) -> int:
        cls_handle, mid_handle = args[1], args[2]
        key = self.handle_table.resolve(mid_handle, HandleType.METHOD_ID)
        handler = self.static_methods.get(key)
        if handler:
            obj = handler(args[3:])
            return self.handle_table.allocate(HandleType.LOCAL_REF, obj)
        return 0

    def _jni_NewStringUTF(self, args: List[int]) -> int:
        bytes_ptr = args[1]
        s = self.read_guest_string(bytes_ptr)
        return self.handle_table.allocate(HandleType.LOCAL_REF, s)

    def _jni_GetStringUTFChars(self, args: List[int]) -> int:
        str_handle = args[1]
        s = self.handle_table.resolve(str_handle)
        guest_ptr = self.engine.write_string(s, encoding="utf-8")
        return guest_ptr

    def _jni_ReleaseStringUTFChars(self, args: List[int]) -> int:
        return 0

    # --------------------------------------------------------------------------
    # Host Invocation of JNI Exports in libHeroesLore5.so
    # --------------------------------------------------------------------------

    def invoke_nativeInitAudioConfiguration(self, threshold_pcm: int) -> None:
        """
        Simplest startup-relevant JNI export:
          void Java_co_kr_eamobile_CletEntry_nativeInitAudioConfiguration(JNIEnv* env, jobject thiz, jlong threshold)
        Takes 64-bit jlong threshold across ARM32 EABI (split across R2, R3).
        """
        self.handle_table.push_local_frame()
        try:
            thiz_handle = self.handle_table.allocate(HandleType.LOCAL_REF, "CletEntry_instance")
            low32 = threshold_pcm & 0xFFFFFFFF
            high32 = (threshold_pcm >> 32) & 0xFFFFFFFF
            # R0 = env32, R1 = thiz, R2 = low32, R3 = high32
            self.engine.call_function(
                "Java_co_kr_eamobile_CletEntry_nativeInitAudioConfiguration",
                self.jnienv32_ptr,
                thiz_handle,
                low32,
                high32,
            )
        finally:
            self.handle_table.pop_local_frame()

    def invoke_nativeInitKernel(self) -> None:
        """
        Startup JNI Call #1:
          void Java_co_kr_eamobile_CletEntry_nativeInitKernel(JNIEnv* env, jobject thiz)
        Executes AndroidService::makeInstance(env) and caches static method IDs.
        """
        self.handle_table.push_local_frame()
        try:
            thiz_handle = self.handle_table.allocate(HandleType.LOCAL_REF, "CletEntry_instance")
            self.engine.call_function(
                "Java_co_kr_eamobile_CletEntry_nativeInitKernel",
                self.jnienv32_ptr,
                thiz_handle,
            )
        finally:
            self.handle_table.pop_local_frame()
    def invoke_nativeInitFileStorage(self, internal_path: str, external_path: str) -> None:
        """
        Startup JNI Call #3:
          void Java_co_kr_eamobile_CletEntry_nativeInitFileStorage(JNIEnv* env, jobject thiz, jstring internalPath, jstring externalPath)
        """
        self.handle_table.push_local_frame()
        try:
            thiz_handle = self.handle_table.allocate(HandleType.LOCAL_REF, "CletEntry_instance")
            int_str = self.handle_table.allocate(HandleType.LOCAL_REF, internal_path, class_name="java/lang/String")
            ext_str = self.handle_table.allocate(HandleType.LOCAL_REF, external_path, class_name="java/lang/String")
            self.engine.call_function(
                "Java_co_kr_eamobile_CletEntry_nativeInitFileStorage",
                self.jnienv32_ptr,
                thiz_handle,
                int_str,
                ext_str,
            )
        finally:
            self.handle_table.pop_local_frame()

    def invoke_getInternalFsAbsPath(self) -> str:
        km_inst = self.engine.call_function("_ZN18MIDASKernelManager11getInstanceEv")
        ptr = self.engine.call_function("_ZN18MIDASKernelManager20getInternalFsAbsPathEv", km_inst)
        return self.read_guest_string(ptr)

    def invoke_getExternalFsAbsPath(self) -> str:
        km_inst = self.engine.call_function("_ZN18MIDASKernelManager11getInstanceEv")
        ptr = self.engine.call_function("_ZN18MIDASKernelManager20getExternalFsAbsPathEv", km_inst)
        return self.read_guest_string(ptr)


    def invoke_nativeInitDisplay(
        self,
        screen_w: int = 400,
        screen_h: int = 240,
        red: int = 8,
        green: int = 8,
        blue: int = 8,
        alpha: int = 8,
        wipi_w: int = 400,
        wipi_h: int = 240,
        margin_x: int = 0,
        margin_y: int = 0,
        wipi_color: int = 0,
        magnification: float = 1.0,
        filter_type: int = 0,
        view_mode: int = 0,
        use_sublayer: int = 0,
        sublayer_pixel: int = 0,
    ) -> int:
        """
        Startup JNI Call #2:
          jint Java_co_kr_eamobile_CletEntry_nativeInitDisplay(...)
        Passes display configuration into MIDASKernelManager and returns autoMarginY.
        """
        self.handle_table.push_local_frame()
        try:
            thiz_handle = self.handle_table.allocate(HandleType.LOCAL_REF, "CletEntry_instance")
            # Pack float as uint32 bits
            mag_bits = struct.unpack("<I", struct.pack("<f", magnification))[0]

            # In ARM32 EABI, first 4 args in R0..R3, remaining args on stack!
            stack_args = [
                red, green, blue, alpha,
                wipi_w, wipi_h, margin_x, margin_y,
                wipi_color, mag_bits, filter_type, view_mode,
                use_sublayer, sublayer_pixel
            ]

            # Push stack args in reverse onto the guest stack
            sp = STACK_BASE + STACK_SIZE - 0x1000 - (len(stack_args) * 4)
            for idx, arg_val in enumerate(stack_args):
                self.engine.uc.mem_write(sp + (idx * 4), struct.pack("<I", arg_val & 0xFFFFFFFF))
            self.engine.uc.reg_write(UC_ARM_REG_SP, sp)

            ret_val = self.engine.call_function(
                "Java_co_kr_eamobile_CletEntry_nativeInitDisplay",
                self.jnienv32_ptr,
                thiz_handle,
                screen_w,
                screen_h,
            )
            return ret_val
        finally:
            self.handle_table.pop_local_frame()

    def invoke_nativeInitVFS(self, guest_fd: int, start_offset: int, length: int) -> None:
        """
        Startup JNI Call #4:
          void Java_co_kr_eamobile_CletEntry_nativeInitVFS(JNIEnv* env, jobject thiz, jobject fileDescriptor, jlong offset, jlong length)
        """
        self.handle_table.push_local_frame()
        try:
            thiz_handle = self.handle_table.allocate(HandleType.LOCAL_REF, "CletEntry_instance")
            fd_obj = {"class": "java/io/FileDescriptor", "descriptor": guest_fd}
            fd_handle = self.handle_table.allocate(HandleType.LOCAL_REF, fd_obj, class_name="java/io/FileDescriptor")

            # Stack args for nativeInitVFS in ARM32 EABI:
            # R0 = env, R1 = thiz, R2 = fd_handle, R3 = 0 (alignment)
            # [sp + 0] = offset_low
            # [sp + 4] = offset_high
            # [sp + 8] = length_low
            # [sp + 12] = length_high
            offset_low = start_offset & 0xFFFFFFFF
            offset_high = (start_offset >> 32) & 0xFFFFFFFF
            length_low = length & 0xFFFFFFFF
            length_high = (length >> 32) & 0xFFFFFFFF

            self.engine.call_function(
                "Java_co_kr_eamobile_CletEntry_nativeInitVFS",
                self.jnienv32_ptr,
                thiz_handle,
                fd_handle,
                0,
                offset_low,
                offset_high,
                length_low,
                length_high,
            )
        finally:
            self.handle_table.pop_local_frame()

    def invoke_getAssetSizeFromVFS(self, asset_name: str) -> Tuple[int, int]:
        """
        Calls MIDASKernelManager::getAssetSizeFromVFS(char const* asset_name, unsigned long* size_out)
        Returns (size, payload_offset).
        """
        s_ptr = self.engine.write_string(asset_name)
        out_ptr = self.engine.heap_curr
        self.engine.heap_curr += 8
        self.engine.uc.mem_write(out_ptr, b"\x00" * 8)

        km_inst = self.engine.call_function("_ZN18MIDASKernelManager11getInstanceEv")
        size = self.engine.call_function(
            "_ZN18MIDASKernelManager19getAssetSizeFromVFSEPKcPm",
            km_inst,
            s_ptr,
            out_ptr,
        )
        payload_offset = struct.unpack("<I", self.engine.uc.mem_read(out_ptr, 4))[0]
        return size, payload_offset
