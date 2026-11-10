/* rdr_gl.c - see rdr_gl.h. */
#include "rdr_gl.h"

#if defined(_WIN32)
#include <windows.h>
static void *proc(const char *name)
{
    static HMODULE gl;
    if (!gl)
        gl = LoadLibraryExA("opengl32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);   /* the system's, not one dropped beside the exe */
        if (!gl) gl = LoadLibraryA("opengl32.dll");                              /* (Windows 7 without KB2533623) */
    return gl ? (void *)GetProcAddress(gl, name) : NULL;
}
#else
#include <SDL.h>
static void *proc(const char *name)
{
    return SDL_GL_GetProcAddress(name);
}
#endif

RdrGl rdr_gl;

int rdr_gl_load(void)
{
#define L(f) if (!(*(void **)&rdr_gl.f = proc("gl" #f))) return 0;
    L(GenTextures) L(DeleteTextures) L(BindTexture) L(TexImage2D) L(TexParameteri)
    L(PixelStorei) L(GetIntegerv) L(Viewport) L(ClearColor) L(Clear) L(ReadPixels)
#undef L
    return 1;
}
