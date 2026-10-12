#ifndef HL5_GLES_BRIDGE_H
#define HL5_GLES_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string>
#include <vector>
#include <cstring>

namespace hl5 {

class Arm32NativeEngine;

// Typed GLES 1.1 function pointer prototypes
typedef void (*PFN_glBindTexture)(uint32_t target, uint32_t texture);
typedef void (*PFN_glClear)(uint32_t mask);
typedef void (*PFN_glClearColor)(float red, float green, float blue, float alpha);
typedef void (*PFN_glColor4f)(float red, float green, float blue, float alpha);
typedef void (*PFN_glCullFace)(uint32_t mode);
typedef void (*PFN_glDeleteTextures)(int32_t n, const uint32_t* textures);
typedef void (*PFN_glDisable)(uint32_t cap);
typedef void (*PFN_glDisableClientState)(uint32_t array);
typedef void (*PFN_glDrawArrays)(uint32_t mode, int32_t first, int32_t count);
typedef void (*PFN_glEnable)(uint32_t cap);
typedef void (*PFN_glEnableClientState)(uint32_t array);
typedef void (*PFN_glGenTextures)(int32_t n, uint32_t* textures);
typedef void (*PFN_glLoadIdentity)(void);
typedef void (*PFN_glMatrixMode)(uint32_t mode);
typedef void (*PFN_glOrthof)(float left, float right, float bottom, float top, float zNear, float zFar);
typedef void (*PFN_glTexCoordPointer)(int32_t size, uint32_t type, int32_t stride, const void* pointer);
typedef void (*PFN_glTexImage2D)(uint32_t target, int32_t level, int32_t internalformat, int32_t width, int32_t height, int32_t border, uint32_t format, uint32_t type, const void* pixels);
typedef void (*PFN_glTexParameterf)(uint32_t target, uint32_t pname, float param);
typedef void (*PFN_glTexSubImage2D)(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width, int32_t height, uint32_t format, uint32_t type, const void* pixels);
typedef void (*PFN_glVertexPointer)(int32_t size, uint32_t type, int32_t stride, const void* pointer);
typedef void (*PFN_glViewport)(int32_t x, int32_t y, int32_t width, int32_t height);

struct GlesDiagnostics {
    uint32_t total_calls = 0;
    uint32_t draw_arrays_calls = 0;
    uint32_t bind_texture_calls = 0;
    uint32_t gen_textures_calls = 0;
    uint32_t last_bound_texture = 0;
    int32_t viewport_w = 0;
    int32_t viewport_h = 0;
    float last_ortho_left = 0.0f;
    float last_ortho_right = 0.0f;
    float last_ortho_bottom = 0.0f;
    float last_ortho_top = 0.0f;
};

/**
 * GlesBridge
 * Bridges the 19 GLES 1.1 CM functions between 32-bit ARM guest and 64-bit AArch64 host.
 *
 * Guarantees:
 *  1. ARM32 EABI Softfp float unpacking (IEEE-754 bit conversion).
 *  2. Stack argument decoding for calls with >4 parameters (glOrthof, glTexImage2D).
 *  3. Safe guest pointer marshaling for buffers (glVertexPointer, glTexCoordPointer, etc.).
 *  4. Zero 64-bit host pointer leakage into guest memory.
 *  5. Observable execution diagnostics and draw call counters.
 */
class GlesBridge {
public:
    GlesBridge();
    ~GlesBridge();

    bool initialize();
    bool is_gles1_supported() const { return gles1_supported_; }
    const char* gles_provider_name() const { return provider_name_; }

    void register_with_engine(Arm32NativeEngine& engine);

    const GlesDiagnostics& diagnostics() const { return diag_; }
    void reset_diagnostics();
    void dump_status() const;
    void set_surface_size(int32_t width, int32_t height) {
        surface_w_ = width;
        surface_h_ = height;
        apply_active_viewport();
    }
    int32_t surface_width() const { return surface_w_; }
    int32_t surface_height() const { return surface_h_; }

    void set_stretch_mode(bool stretch) {
        stretch_mode_ = stretch;
        apply_active_viewport();
    }
    bool stretch_mode() const { return stretch_mode_; }

    void get_active_viewport(int32_t& out_x, int32_t& out_y, int32_t& out_w, int32_t& out_h) const {
        if (surface_w_ <= 0 || surface_h_ <= 0) {
            out_x = 0;
            out_y = 0;
            out_w = (diag_.viewport_w > 0) ? diag_.viewport_w : 400;
            out_h = (diag_.viewport_h > 0) ? diag_.viewport_h : 240;
            return;
        }
        if (stretch_mode_) {
            out_x = 0;
            out_y = 0;
            out_w = surface_w_;
            out_h = surface_h_;
            return;
        }
        // Original 5:3 (400:240) aspect ratio centered (pillarbox/letterbox)
        if (surface_w_ * 3 > surface_h_ * 5) {
            out_h = surface_h_;
            out_w = (surface_h_ * 5) / 3;
            out_x = (surface_w_ - out_w) / 2;
            out_y = 0;
        } else {
            out_w = surface_w_;
            out_h = (surface_w_ * 3) / 5;
            out_x = 0;
            out_y = (surface_h_ - out_h) / 2;
        }
    }

    void apply_active_viewport();
    void clear_screen();

    static inline float unpack_float(uint32_t u) {
        float f;
        std::memcpy(&f, &u, sizeof(f));
        return f;
    }

private:
    void* lib_handle_;
    bool gles1_supported_;
    const char* provider_name_;
    Arm32NativeEngine* engine_;
    GlesDiagnostics diag_;
    int32_t surface_w_ = 0;
    int32_t surface_h_ = 0;

    bool stretch_mode_ = false; // Default: false (5:3 Original Aspect Ratio Centered)
    PFN_glClear              p_glClear = nullptr;
    PFN_glClearColor         p_glClearColor = nullptr;
    // Host GLES 1.1 Function Pointers
    PFN_glBindTexture        p_glBindTexture;
    PFN_glColor4f            p_glColor4f;
    PFN_glCullFace           p_glCullFace;
    PFN_glDeleteTextures     p_glDeleteTextures;
    PFN_glDisable            p_glDisable;
    PFN_glDisableClientState p_glDisableClientState;
    PFN_glDrawArrays         p_glDrawArrays;
    PFN_glEnable             p_glEnable;
    PFN_glEnableClientState  p_glEnableClientState;
    PFN_glGenTextures        p_glGenTextures;
    PFN_glLoadIdentity       p_glLoadIdentity;
    PFN_glMatrixMode         p_glMatrixMode;
    PFN_glOrthof             p_glOrthof;
    PFN_glTexCoordPointer    p_glTexCoordPointer;
    PFN_glTexImage2D         p_glTexImage2D;
    PFN_glTexParameterf      p_glTexParameterf;
    PFN_glTexSubImage2D      p_glTexSubImage2D;
    PFN_glVertexPointer      p_glVertexPointer;
    PFN_glViewport           p_glViewport;

    // Buffer shadow caches for safe pointer passing to host driver
    uint32_t guest_vertex_ptr_;
    int32_t  vertex_size_;
    uint32_t vertex_type_;
    int32_t  vertex_stride_;
    std::vector<uint8_t> host_vertex_buffer_;

    uint32_t guest_texcoord_ptr_;
    int32_t  texcoord_size_;
    uint32_t texcoord_type_;
    int32_t  texcoord_stride_;
    std::vector<uint8_t> host_texcoord_buffer_;
};

} // namespace hl5

#endif // HL5_GLES_BRIDGE_H
