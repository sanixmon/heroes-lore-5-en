#ifndef HL5_GLES_BRIDGE_H
#define HL5_GLES_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

namespace hl5 {

/**
 * GlesBridge
 * Manages the 19 OpenGL ES 1.1 entry points required by libHeroesLore5.so:
 *   1.  glBindTexture
 *   2.  glColor4f
 *   3.  glCullFace
 *   4.  glDeleteTextures
 *   5.  glDisable
 *   6.  glDisableClientState
 *   7.  glDrawArrays
 *   8.  glEnable
 *   9.  glEnableClientState
 *   10. glGenTextures
 *   11. glLoadIdentity
 *   12. glMatrixMode
 *   13. glOrthof
 *   14. glTexCoordPointer
 *   15. glTexImage2D
 *   16. glTexParameterf
 *   17. glTexSubImage2D
 *   18. glVertexPointer
 *   19. glViewport
 */
class GlesBridge {
public:
    GlesBridge();
    ~GlesBridge();

    bool initialize();
    bool is_gles1_supported() const { return gles1_supported_; }
    const char* gles_provider_name() const { return provider_name_; }

    // Diagnostic information
    void dump_status() const;

private:
    void* lib_handle_;
    bool gles1_supported_;
    const char* provider_name_;

    // Pointers for 19 required GLES 1.1 CM functions
    void* p_glBindTexture;
    void* p_glColor4f;
    void* p_glCullFace;
    void* p_glDeleteTextures;
    void* p_glDisable;
    void* p_glDisableClientState;
    void* p_glDrawArrays;
    void* p_glEnable;
    void* p_glEnableClientState;
    void* p_glGenTextures;
    void* p_glLoadIdentity;
    void* p_glMatrixMode;
    void* p_glOrthof;
    void* p_glTexCoordPointer;
    void* p_glTexImage2D;
    void* p_glTexParameterf;
    void* p_glTexSubImage2D;
    void* p_glVertexPointer;
    void* p_glViewport;
};

} // namespace hl5

#endif // HL5_GLES_BRIDGE_H
