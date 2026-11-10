/*
 * rdr_gl.h - the few GL 1.1 calls the UI makes itself (textures, clears,
 * a screenshot). Everything else goes through Dear ImGui's GL3 back end,
 * which loads its own. Filled by rdr_gl_load() once a context is current.
 */
#ifndef RDR_GL_H
#define RDR_GL_H

#include <stddef.h>

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;

#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D          0x0DE1
#define GL_RGBA                0x1908
#define GL_UNSIGNED_BYTE       0x1401
#define GL_TEXTURE_MIN_FILTER  0x2801
#define GL_TEXTURE_MAG_FILTER  0x2800
#define GL_TEXTURE_WRAP_S      0x2802
#define GL_TEXTURE_WRAP_T      0x2803
#define GL_LINEAR              0x2601
#define GL_REPEAT              0x2901
#define GL_UNPACK_ALIGNMENT    0x0CF5
#define GL_UNPACK_ROW_LENGTH   0x0CF2
#define GL_PACK_ALIGNMENT      0x0D05
#define GL_COLOR_BUFFER_BIT    0x00004000
#define GL_TEXTURE_BINDING_2D  0x8069
#define GL_BGRA                0x80E1
#endif

#if defined(_WIN32)
#define RDR_GLAPI __stdcall
#else
#define RDR_GLAPI
#endif

typedef struct {
    void (RDR_GLAPI *GenTextures)(GLsizei, GLuint *);
    void (RDR_GLAPI *DeleteTextures)(GLsizei, const GLuint *);
    void (RDR_GLAPI *BindTexture)(GLenum, GLuint);
    void (RDR_GLAPI *TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (RDR_GLAPI *TexParameteri)(GLenum, GLenum, GLint);
    void (RDR_GLAPI *PixelStorei)(GLenum, GLint);
    void (RDR_GLAPI *GetIntegerv)(GLenum, GLint *);
    void (RDR_GLAPI *Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (RDR_GLAPI *ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (RDR_GLAPI *Clear)(GLbitfield);
    void (RDR_GLAPI *ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
} RdrGl;

#ifdef __cplusplus
extern "C" {
#endif
extern RdrGl rdr_gl;
int rdr_gl_load(void);
#ifdef __cplusplus
}
#endif

#endif
