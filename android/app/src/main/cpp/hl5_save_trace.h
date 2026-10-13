#ifndef HL5_SAVE_TRACE_H
#define HL5_SAVE_TRACE_H

// Targeted save/load persistence diagnostics (audit task: physical-device
// save -> force-stop -> relaunch). Read-only logging only:
// - Filesystem hooks log save-related paths (LOCAL.sav, kickass, SL_*.sav).
// - Guest tracepoints log LoadLocalSaveData/SaveSlotData/LoadSlotData and
//   MX_sd* entry/exit plus the gv+0x29 destructive-branch check.
// Nothing here changes guest behavior: no register/memory writes, no branch
// suppression, no extra LoadSlotData calls, no format changes.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <unicorn/unicorn.h>
#include <unicorn/arm.h>

#include "arm32_native_engine.h"
#include "arm32_elf_loader.h"

#ifdef __ANDROID__
#include <android/log.h>
#define HL5_SAVE_LOG(...) __android_log_print(ANDROID_LOG_INFO, "HL5_Save", __VA_ARGS__)
#else
#define HL5_SAVE_LOG(...) \
    do { \
        fprintf(stderr, "[HL5_Save] "); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } while (0)
#endif

// Save-related path predicate shared by the POSIX hooks. Everything else
// stays on the existing (Android-only, low-volume) log path.
static inline bool hl5_is_save_path(const std::string& p) {
    if (p.find("LOCAL.sav") != std::string::npos) return true;
    if (p.find("kickass") != std::string::npos) return true;
    if (p.find("SL_") != std::string::npos) return true;
    if (p.size() >= 4 && p.compare(p.size() - 4, 4, ".sav") == 0) return true;
    return false;
}

// ---- Read-only guest tracepoints -------------------------------------------
// Link-time guest addresses (runtime = loader base + link). Deltas below were
// verified against apktool_modern/lib/armeabi/libHeroesLore5.so disassembly:
//   LoadLocalSaveData entry 0x77f94, exit 0x7807c, fallback-init call 0x78088,
//     gv+0x29 branch check 0x78054
//   SaveSlotData entry 0x8c804, R0-valid exit 0x8d00c (covers early+normal)
//   LoadSlotData entry 0x8c40c, R0-valid exit 0x8c7c0 (R0 = size-16)
//   MX_sdGetSize entry 0x40058, exit 0x40098
//   MX_sdSaveData entry 0x40794, exit 0x40844
// UC_HOOK_CODE fires before the instruction executes, so exit hooks target
// the first address where R0 already holds the return value.

static const char* Hl5Tag_LoadLocalEnter = "enter:CommonUi::LoadLocalSaveData";
static const char* Hl5Tag_LoadLocalExit = "exit:CommonUi::LoadLocalSaveData";
static const char* Hl5Tag_LoadLocalFallback = "fallback:LoadLocal:FileRead-failed:Init+rewrite";
static const char* Hl5Tag_LoadLocalBranch = "check:LoadLocal:gv+0x29-vs-kickass-remove";
static const char* Hl5Tag_SaveSlotEnter = "enter:SlotInfo::SaveSlotData";
static const char* Hl5Tag_SaveSlotExit = "exit:SlotInfo::SaveSlotData";
static const char* Hl5Tag_LoadSlotEnter = "enter:SlotInfo::LoadSlotData";
static const char* Hl5Tag_LoadSlotExit = "exit:SlotInfo::LoadSlotData";
static const char* Hl5Tag_SdGetSizeEnter = "enter:MX_sdGetSize";
static const char* Hl5Tag_SdGetSizeExit = "exit:MX_sdGetSize";
static const char* Hl5Tag_SdSaveEnter = "enter:MX_sdSaveData";
static const char* Hl5Tag_SdSaveExit = "exit:MX_sdSaveData";

namespace {
struct Hl5SaveTraceState {
    uint32_t gv_addr = 0;
    uint32_t load_slot_obj = 0;
    int load_slot_idx = -1;
    int8_t load_slot_before = 0;
    bool have_before = false;
    std::unordered_map<uint32_t, const char*> tags;
    std::vector<uc_hook> hooks;
};
static Hl5SaveTraceState g_hl5_save_trace;
} // namespace

static void Hl5SaveTrace_Hook(uc_engine* uc, uint64_t address, uint32_t /*size*/, void* user_data) {
    hl5::Arm32NativeEngine* eng = static_cast<hl5::Arm32NativeEngine*>(user_data);
    if (eng == nullptr) return;
    auto it = g_hl5_save_trace.tags.find(static_cast<uint32_t>(address));
    const char* tag = (it != g_hl5_save_trace.tags.end()) ? it->second : "?";
    uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, lr = 0;
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    uc_reg_read(uc, UC_ARM_REG_R1, &r1);
    uc_reg_read(uc, UC_ARM_REG_R2, &r2);
    uc_reg_read(uc, UC_ARM_REG_R3, &r3);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);

    if (tag == Hl5Tag_SdGetSizeEnter) {
        std::string name = eng->read_string(r0);
        HL5_SAVE_LOG("%s name=\"%s\"", tag, name.c_str());
        return;
    }
    if (tag == Hl5Tag_LoadSlotEnter) {
        g_hl5_save_trace.load_slot_obj = r0;
        g_hl5_save_trace.load_slot_idx = static_cast<int8_t>(r1);
        g_hl5_save_trace.have_before = false;
        int8_t before = 0;
        if (r0 >= 0x10000 && eng->read_memory(r0, &before, 1)) {
            g_hl5_save_trace.load_slot_before = before;
            g_hl5_save_trace.have_before = true;
        }
        HL5_SAVE_LOG("%s slot=%d obj=0x%08x class_before=%d", tag,
                     static_cast<int>(static_cast<int8_t>(r1)), r0,
                     g_hl5_save_trace.have_before ? (int)g_hl5_save_trace.load_slot_before : -999);
        return;
    }
    if (tag == Hl5Tag_LoadSlotExit) {
        int32_t ret = static_cast<int32_t>(r0);
        int8_t after = 0;
        bool have_after = false;
        if (g_hl5_save_trace.load_slot_obj >= 0x10000 &&
            eng->read_memory(g_hl5_save_trace.load_slot_obj, &after, 1)) {
            have_after = true;
        }
        HL5_SAVE_LOG("%s slot=%d ret=%d early_return(size<=16)=%s class_before=%d class_after=%d", tag,
                     g_hl5_save_trace.load_slot_idx, ret, (ret <= 0 ? "YES" : "no"),
                     g_hl5_save_trace.have_before ? (int)g_hl5_save_trace.load_slot_before : -999,
                     have_after ? (int)after : -999);
        g_hl5_save_trace.have_before = false;
        return;
    }
    if (tag == Hl5Tag_LoadLocalBranch) {
        if (g_hl5_save_trace.gv_addr == 0) {
            HL5_SAVE_LOG("%s gv_unknown(cannot resolve symbol gv)", tag);
            return;
        }
        int8_t flag = 0;
        if (!eng->read_memory(g_hl5_save_trace.gv_addr + 0x29, &flag, 1)) {
            HL5_SAVE_LOG("%s gv+0x29 unreadable", tag);
            return;
        }
        HL5_SAVE_LOG("%s gv+0x29=%d predicts_MC_fsRemove(kickass)=%s", tag, (int)flag,
                     (flag != 0 ? "WILL-FIRE" : "will-not-fire"));
        return;
    }
    if (tag == Hl5Tag_LoadLocalExit) {
        HL5_SAVE_LOG("%s ret=%d(FileRead-derived;<=0 means fallback Init+rewrite ran)", tag,
                     static_cast<int32_t>(r0));
        return;
    }
    if (tag == Hl5Tag_SaveSlotEnter) {
        HL5_SAVE_LOG("%s slot=%d obj=0x%08x", tag, static_cast<int>(static_cast<int8_t>(r1)), r0);
        return;
    }
    // Generic entry/exit: log registers only.
    HL5_SAVE_LOG("%s r0=0x%08x r1=0x%08x r2=0x%08x lr=0x%08x", tag, r0, r1, r2, lr);
}

static void Hl5SaveTrace_Add(hl5::Arm32NativeEngine& eng, uint32_t vaddr, const char* tag) {
    if (vaddr == 0 || tag == nullptr) return;
    g_hl5_save_trace.tags[vaddr] = tag;
    uc_hook h = 0;
    uc_err err = uc_hook_add(eng.uc_, &h, UC_HOOK_CODE,
                             reinterpret_cast<void*>(Hl5SaveTrace_Hook), &eng,
                             vaddr, vaddr);
    if (err == UC_ERR_OK) {
        g_hl5_save_trace.hooks.push_back(h);
    }
}

// Resolves guest symbols and installs the tracepoints. Best-effort: any
// missing symbol skips its group with a one-line note. No guest state touched.
static void Hl5SaveTrace_Install(hl5::Arm32NativeEngine& eng, hl5::Arm32ElfLoader& ldr) {
    auto sym = [&ldr](const char* n) -> uint32_t { return ldr.find_symbol(n); };
    g_hl5_save_trace.gv_addr = sym("gv");
    HL5_SAVE_LOG("storage-trace init: gv=0x%08x", g_hl5_save_trace.gv_addr);

    uint32_t load_local = sym("_ZN8CommonUi17LoadLocalSaveDataEv");
    if (load_local != 0) {
        Hl5SaveTrace_Add(eng, load_local, Hl5Tag_LoadLocalEnter);
        Hl5SaveTrace_Add(eng, load_local + 0xE8, Hl5Tag_LoadLocalExit);
        Hl5SaveTrace_Add(eng, load_local + 0xF4, Hl5Tag_LoadLocalFallback);
        Hl5SaveTrace_Add(eng, load_local + 0xC0, Hl5Tag_LoadLocalBranch);
    } else {
        HL5_SAVE_LOG("tracepoint group LoadLocalSaveData skipped (symbol missing)");
    }
    uint32_t save_slot = sym("_ZN8SlotInfo12SaveSlotDataEa");
    if (save_slot != 0) {
        Hl5SaveTrace_Add(eng, save_slot, Hl5Tag_SaveSlotEnter);
        Hl5SaveTrace_Add(eng, save_slot + 0x808, Hl5Tag_SaveSlotExit);
    } else {
        HL5_SAVE_LOG("tracepoint group SaveSlotData skipped (symbol missing)");
    }
    uint32_t load_slot = sym("_ZN8SlotInfo12LoadSlotDataEa");
    if (load_slot != 0) {
        Hl5SaveTrace_Add(eng, load_slot, Hl5Tag_LoadSlotEnter);
        Hl5SaveTrace_Add(eng, load_slot + 0x3B4, Hl5Tag_LoadSlotExit);
    } else {
        HL5_SAVE_LOG("tracepoint group LoadSlotData skipped (symbol missing)");
    }
    uint32_t get_size = sym("MX_sdGetSize");
    if (get_size != 0) {
        Hl5SaveTrace_Add(eng, get_size, Hl5Tag_SdGetSizeEnter);
        Hl5SaveTrace_Add(eng, get_size + 0x40, Hl5Tag_SdGetSizeExit);
    } else {
        HL5_SAVE_LOG("tracepoint group MX_sdGetSize skipped (symbol missing)");
    }
    uint32_t sd_save = sym("MX_sdSaveData");
    if (sd_save != 0) {
        Hl5SaveTrace_Add(eng, sd_save, Hl5Tag_SdSaveEnter);
        Hl5SaveTrace_Add(eng, sd_save + 0xB0, Hl5Tag_SdSaveExit);
    } else {
        HL5_SAVE_LOG("tracepoint group MX_sdSaveData skipped (symbol missing)");
    }
    HL5_SAVE_LOG("storage-trace installed: %u tracepoints", (unsigned)g_hl5_save_trace.tags.size());
}

#endif // HL5_SAVE_TRACE_H
