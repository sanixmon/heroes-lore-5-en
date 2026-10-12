#include "gles_bridge.h"
#include "arm32_native_engine.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "HL5_GlesBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do {} while(0)
#define LOGW(...) do {} while(0)
#define LOGE(...) do {} while(0)
#endif
namespace hl5 {

GlesBridge::GlesBridge()
    : lib_handle_(nullptr),
      gles1_supported_(false),
      provider_name_("None"),
      engine_(nullptr),
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
      p_glViewport(nullptr),
      guest_vertex_ptr_(0),
      vertex_size_(2),
      vertex_type_(0x1406), // GL_FLOAT
      vertex_stride_(0),
      guest_texcoord_ptr_(0),
      texcoord_size_(2),
      texcoord_type_(0x1406),
      texcoord_stride_(0) {}

GlesBridge::~GlesBridge() {
    if (lib_handle_) {
        dlclose(lib_handle_);
    }
}

bool GlesBridge::initialize() {
    // Attempt to load native system GLES 1.1 CM library
    const char* candidates[] = {
        "libGLESv1_CM.so",
        "/system/lib64/libGLESv1_CM.so",
        "/system/lib/libGLESv1_CM.so",
        nullptr
    };

    for (int i = 0; candidates[i] != nullptr; ++i) {
        lib_handle_ = dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL);
        if (lib_handle_) {
            provider_name_ = candidates[i];
            LOGI("Loaded GLES provider: %s", candidates[i]);
            break;
        }
    }

    if (!lib_handle_) {
        provider_name_ = "Missing / Needs GL4ES Fallback";
        gles1_supported_ = false;
        LOGW("Failed to load libGLESv1_CM.so from system candidates; GLES1 calls will safely no-op");
        return false;
    }

    #define RESOLVE_GL(type, name) \
        p_##name = reinterpret_cast<type>(dlsym(lib_handle_, #name)); \
        if (!p_##name) { gles1_supported_ = false; return false; }

    RESOLVE_GL(PFN_glBindTexture,        glBindTexture);
    RESOLVE_GL(PFN_glColor4f,            glColor4f);
    RESOLVE_GL(PFN_glCullFace,           glCullFace);
    RESOLVE_GL(PFN_glDeleteTextures,     glDeleteTextures);
    RESOLVE_GL(PFN_glDisable,            glDisable);
    RESOLVE_GL(PFN_glDisableClientState, glDisableClientState);
    RESOLVE_GL(PFN_glDrawArrays,         glDrawArrays);
    RESOLVE_GL(PFN_glEnable,             glEnable);
    RESOLVE_GL(PFN_glEnableClientState,  glEnableClientState);
    RESOLVE_GL(PFN_glGenTextures,        glGenTextures);
    RESOLVE_GL(PFN_glLoadIdentity,       glLoadIdentity);
    RESOLVE_GL(PFN_glMatrixMode,         glMatrixMode);
    RESOLVE_GL(PFN_glOrthof,             glOrthof);
    RESOLVE_GL(PFN_glTexCoordPointer,    glTexCoordPointer);
    RESOLVE_GL(PFN_glTexImage2D,         glTexImage2D);
    RESOLVE_GL(PFN_glTexParameterf,      glTexParameterf);
    RESOLVE_GL(PFN_glTexSubImage2D,      glTexSubImage2D);
    RESOLVE_GL(PFN_glVertexPointer,      glVertexPointer);
    RESOLVE_GL(PFN_glViewport,           glViewport);

    #undef RESOLVE_GL

    gles1_supported_ = true;
    return true;
}

void GlesBridge::reset_diagnostics() {
    diag_ = GlesDiagnostics{};
}

void GlesBridge::dump_status() const {
    printf("[GlesBridge] Provider: %s, Supported: %s, Calls: %u, Draws: %u, Textures: %u, Viewport: %dx%d\n",
           provider_name_, gles1_supported_ ? "YES" : "NO",
           diag_.total_calls, diag_.draw_arrays_calls, diag_.bind_texture_calls,
           diag_.viewport_w, diag_.viewport_h);
}

void GlesBridge::register_with_engine(Arm32NativeEngine& engine) {
    engine_ = &engine;

    engine.register_host_hook("glEnable", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        if (p_glEnable) p_glEnable(args[0]);
        return 0;
    });

    engine.register_host_hook("glDisable", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        if (p_glDisable) p_glDisable(args[0]);
        return 0;
    });

    engine.register_host_hook("glEnableClientState", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        if (p_glEnableClientState) p_glEnableClientState(args[0]);
        return 0;
    });

    engine.register_host_hook("glDisableClientState", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        if (p_glDisableClientState) p_glDisableClientState(args[0]);
        return 0;
    });

    engine.register_host_hook("glMatrixMode", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        if (p_glMatrixMode) p_glMatrixMode(args[0]);
        return 0;
    });

    engine.register_host_hook("glLoadIdentity", [this](const std::vector<uint32_t>&) -> uint32_t {
        diag_.total_calls++;
        if (p_glLoadIdentity) p_glLoadIdentity();
        return 0;
    });

    engine.register_host_hook("glCullFace", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        if (p_glCullFace) p_glCullFace(args[0]);
        return 0;
    });

    engine.register_host_hook("glViewport", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        int32_t vx = static_cast<int32_t>(args[0]);
        int32_t vy = static_cast<int32_t>(args[1]);
        int32_t vw = static_cast<int32_t>(args[2]);
        int32_t vh = static_cast<int32_t>(args[3]);
        diag_.viewport_w = (surface_w_ > 0) ? surface_w_ : vw;
        diag_.viewport_h = (surface_h_ > 0) ? surface_h_ : vh;

        // When physical surface size is configured, enforce true fullscreen viewport across the device:
        int32_t target_x = (surface_w_ > 0) ? 0 : vx;
        int32_t target_y = (surface_h_ > 0) ? 0 : vy;
        int32_t target_w = (surface_w_ > 0) ? surface_w_ : vw;
        int32_t target_h = (surface_h_ > 0) ? surface_h_ : vh;

        if (p_glViewport) {
            p_glViewport(target_x, target_y, target_w, target_h);
        }
        return 0;
    });

    engine.register_host_hook("glBindTexture", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        diag_.bind_texture_calls++;
        diag_.last_bound_texture = args[1];
        if (p_glBindTexture) p_glBindTexture(args[0], args[1]);
        return 0;
    });

    engine.register_host_hook("glGenTextures", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        diag_.gen_textures_calls++;
        int32_t n = static_cast<int32_t>(args[0]);
        uint32_t guest_ptr = args[1];
        if (n > 0 && guest_ptr >= 0x10000) {
            std::vector<uint32_t> ids(n, 0);
            if (p_glGenTextures) {
                p_glGenTextures(n, ids.data());
            } else {
                static uint32_t synthetic_tex_id = 1;
                for (int32_t i = 0; i < n; ++i) ids[i] = synthetic_tex_id++;
            }
            engine_->write_memory(guest_ptr, ids.data(), n * sizeof(uint32_t));
        }
        return 0;
    });

    engine.register_host_hook("glDeleteTextures", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        int32_t n = static_cast<int32_t>(args[0]);
        uint32_t guest_ptr = args[1];
        if (n > 0 && guest_ptr >= 0x10000) {
            std::vector<uint32_t> ids(n, 0);
            engine_->read_memory(guest_ptr, ids.data(), n * sizeof(uint32_t));
            if (p_glDeleteTextures) p_glDeleteTextures(n, ids.data());
        }
        return 0;
    });

    engine.register_host_hook("glTexParameterf", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        float param = unpack_float(args[2]);
        if (p_glTexParameterf) p_glTexParameterf(args[0], args[1], param);
        return 0;
    });

    engine.register_host_hook("glColor4f", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        float r = unpack_float(args[0]);
        float g = unpack_float(args[1]);
        float b = unpack_float(args[2]);
        float a = unpack_float(args[3]);
        if (p_glColor4f) p_glColor4f(r, g, b, a);
        return 0;
    });

    engine.register_host_hook("glOrthof", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        float left   = unpack_float(args[0]);
        float right  = unpack_float(args[1]);
        float bottom = unpack_float(args[2]);
        float top    = unpack_float(args[3]);
        float zNear  = (args.size() > 4) ? unpack_float(args[4]) : -1.0f;
        float zFar   = (args.size() > 5) ? unpack_float(args[5]) :  1.0f;

        // Fullscreen Quad Projection:
        // Texture2D's quad vertices are permanently set to 800.0f x 480.0f (or 400.0f x 240.0f).
        // If the orthographic projection is set to physical surface dimensions (e.g. 1931 x 681),
        // the quad renders as a small rectangle trapped in the bottom-left corner.
        // Lock orthographic projection bounds to match the quad's coordinate space (0..800 x 0..480)
        // so that the GPU hardware scales the quad to fill 100% of the active fullscreen viewport.
        float target_right = (right > 400.0f) ? 800.0f : 400.0f;
        float target_top   = (top > 240.0f) ? 480.0f : 240.0f;

        diag_.last_ortho_left = 0.0f;
        diag_.last_ortho_right = target_right;
        diag_.last_ortho_bottom = 0.0f;
        diag_.last_ortho_top = target_top;

        if (p_glOrthof) p_glOrthof(0.0f, target_right, 0.0f, target_top, zNear, zFar);
        return 0;
    });

    engine.register_host_hook("glVertexPointer", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        vertex_size_   = static_cast<int32_t>(args[0]);
        vertex_type_   = args[1];
        vertex_stride_ = static_cast<int32_t>(args[2]);
        guest_vertex_ptr_ = args[3];
        return 0;
    });

    engine.register_host_hook("glTexCoordPointer", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        texcoord_size_   = static_cast<int32_t>(args[0]);
        texcoord_type_   = args[1];
        texcoord_stride_ = static_cast<int32_t>(args[2]);
        guest_texcoord_ptr_ = args[3];
        return 0;
    });

    engine.register_host_hook("glDrawArrays", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        diag_.draw_arrays_calls++;
        uint32_t mode = args[0];
        int32_t first = static_cast<int32_t>(args[1]);
        int32_t count = static_cast<int32_t>(args[2]);

        // Safely marshal vertex pointer from guest memory to host buffer
        if (guest_vertex_ptr_ >= 0x10000 && count > 0) {
            size_t bytes_per_elem = (vertex_type_ == 0x1406) ? 4 : 2; // GL_FLOAT vs GL_SHORT
            size_t stride = (vertex_stride_ > 0) ? vertex_stride_ : (vertex_size_ * bytes_per_elem);
            size_t total_bytes = (first + count) * stride;

            host_vertex_buffer_.resize(total_bytes);
            engine_->read_memory(guest_vertex_ptr_, host_vertex_buffer_.data(), total_bytes);

            if (p_glVertexPointer) {
                p_glVertexPointer(vertex_size_, vertex_type_, vertex_stride_, host_vertex_buffer_.data());
            }
        }

        // Safely marshal texcoord pointer from guest memory to host buffer
        if (guest_texcoord_ptr_ >= 0x10000 && count > 0) {
            size_t bytes_per_elem = (texcoord_type_ == 0x1406) ? 4 : 2;
            size_t stride = (texcoord_stride_ > 0) ? texcoord_stride_ : (texcoord_size_ * bytes_per_elem);
            size_t total_bytes = (first + count) * stride;

            host_texcoord_buffer_.resize(total_bytes);
            engine_->read_memory(guest_texcoord_ptr_, host_texcoord_buffer_.data(), total_bytes);

            if (p_glTexCoordPointer) {
                p_glTexCoordPointer(texcoord_size_, texcoord_type_, texcoord_stride_, host_texcoord_buffer_.data());
            }
        }

        if (p_glDrawArrays) {
            p_glDrawArrays(mode, first, count);
        }
        return 0;
    });

    engine.register_host_hook("glTexImage2D", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        uint32_t target = args[0];
        int32_t level = static_cast<int32_t>(args[1]);
        int32_t internalformat = static_cast<int32_t>(args[2]);
        int32_t width = static_cast<int32_t>(args[3]);
        int32_t height = (args.size() > 4) ? static_cast<int32_t>(args[4]) : 0;
        int32_t border = (args.size() > 5) ? static_cast<int32_t>(args[5]) : 0;
        uint32_t format = (args.size() > 6) ? args[6] : 0x1908; // GL_RGBA
        uint32_t type = (args.size() > 7) ? args[7] : 0x1401;   // GL_UNSIGNED_BYTE
        uint32_t guest_pixels = (args.size() > 8) ? args[8] : 0;

        const void* host_pixels = nullptr;
        std::vector<uint8_t> pixel_buf;
        if (guest_pixels >= 0x10000 && width > 0 && height > 0) {
            size_t pixel_bytes = width * height * 4; // Assuming RGBA
            pixel_buf.resize(pixel_bytes);
            engine_->read_memory(guest_pixels, pixel_buf.data(), pixel_bytes);
            host_pixels = pixel_buf.data();
        }

        if (p_glTexImage2D) {
            p_glTexImage2D(target, level, internalformat, width, height, border, format, type, host_pixels);
        }
        return 0;
    });

    engine.register_host_hook("glTexSubImage2D", [this](const std::vector<uint32_t>& args) -> uint32_t {
        diag_.total_calls++;
        uint32_t target = args[0];
        int32_t level = static_cast<int32_t>(args[1]);
        int32_t xoffset = static_cast<int32_t>(args[2]);
        int32_t yoffset = static_cast<int32_t>(args[3]);
        int32_t width = (args.size() > 4) ? static_cast<int32_t>(args[4]) : 0;
        int32_t height = (args.size() > 5) ? static_cast<int32_t>(args[5]) : 0;
        uint32_t format = (args.size() > 6) ? args[6] : 0x1908;
        uint32_t type = (args.size() > 7) ? args[7] : 0x1401;
        uint32_t guest_pixels = (args.size() > 8) ? args[8] : 0;

        const void* host_pixels = nullptr;
        std::vector<uint8_t> pixel_buf;
        if (guest_pixels >= 0x10000 && width > 0 && height > 0) {
            size_t pixel_bytes = width * height * 4;
            pixel_buf.resize(pixel_bytes);
            engine_->read_memory(guest_pixels, pixel_buf.data(), pixel_bytes);
            host_pixels = pixel_buf.data();
        }

        if (p_glTexSubImage2D) {
            p_glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, host_pixels);
        }
        return 0;
    });
}

} // namespace hl5
