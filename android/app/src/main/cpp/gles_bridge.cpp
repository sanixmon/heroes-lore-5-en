#include "gles_bridge.h"
#include <dlfcn.h>
#include <cstdio>

namespace hl5 {

GlesBridge::GlesBridge()
    : lib_handle_(nullptr),
      gles1_supported_(false),
      provider_name_("None"),
      p_glBindTexture(nullptr),
      p_glColor4f(nullptr),
      p_glCullFace(nullptr),
      p_glDeleteTextures(nullptr),
      p_glDisable(nullptr),
      p_glDisableClientState(nullptr),
      p_glDrawArrays(nullptr),
      p_glEnable(nullptr),
      p_glEnableClientState(nullptr),
      p_glGenTextures(nullptr),
      p_glLoadIdentity(nullptr),
      p_glMatrixMode(nullptr),
      p_glOrthof(nullptr),
      p_glTexCoordPointer(nullptr),
      p_glTexImage2D(nullptr),
      p_glTexParameterf(nullptr),
      p_glTexSubImage2D(nullptr),
      p_glVertexPointer(nullptr),
      p_glViewport(nullptr) {}

GlesBridge::~GlesBridge() {
    if (lib_handle_) {
        dlclose(lib_handle_);
    }
}

bool GlesBridge::initialize() {
    // Attempt 1: Native system GLES 1.1 CM library
    const char* candidates[] = {
        "libGLESv1_CM.so",
        "/system/lib64/libGLESv1_CM.so",
        "/system/lib/libGLESv1_CM.so",
        nullptr
    };

    for (int i = 0; candidates[i] != nullptr; ++i) {
        lib_handle_ = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (lib_handle_) {
            provider_name_ = candidates[i];
            break;
        }
    }

    if (!lib_handle_) {
        // GLES 1.x is absent (e.g., pure GLES 3.x driver on Android 15/16).
        // GL4ES or custom fixed-function shader emulation required.
        provider_name_ = "Missing / Needs GL4ES Fallback";
        gles1_supported_ = false;
        return false;
    }

    // Resolve all 19 required symbols
    #define RESOLVE_GL(name) \
        p_##name = dlsym(lib_handle_, #name); \
        if (!p_##name) { gles1_supported_ = false; return false; }

    RESOLVE_GL(glBindTexture);
    RESOLVE_GL(glColor4f);
    RESOLVE_GL(glCullFace);
    RESOLVE_GL(glDeleteTextures);
    RESOLVE_GL(glDisable);
    RESOLVE_GL(glDisableClientState);
    RESOLVE_GL(glDrawArrays);
    RESOLVE_GL(glEnable);
    RESOLVE_GL(glEnableClientState);
    RESOLVE_GL(glGenTextures);
    RESOLVE_GL(glLoadIdentity);
    RESOLVE_GL(glMatrixMode);
    RESOLVE_GL(glOrthof);
    RESOLVE_GL(glTexCoordPointer);
    RESOLVE_GL(glTexImage2D);
    RESOLVE_GL(glTexParameterf);
    RESOLVE_GL(glTexSubImage2D);
    RESOLVE_GL(glVertexPointer);
    RESOLVE_GL(glViewport);

    #undef RESOLVE_GL

    gles1_supported_ = true;
    return true;
}

void GlesBridge::dump_status() const {
    printf("[GlesBridge] Provider: %s, GLES 1.1 Supported: %s\n",
           provider_name_, gles1_supported_ ? "YES" : "NO");
}

} // namespace hl5
