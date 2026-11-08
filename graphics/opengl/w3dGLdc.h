#ifndef W3D_GLDC_H
#define W3D_GLDC_H
// ============================================================================
//  w3dGLdc.h -- lo que GLdc (el OpenGL 1.x de la PVR de la Dreamcast, kos-ports/libGL) NO trae y el backend fijo
//  (w3dGraphics.cpp) nombra. Solo con W3D_DREAMCAST.
//
//  GLdc es un pipeline fijo como el de GL 1.x / GLES 1.1, pero sin: buffer objects, glBlendEquation, texgen, planos de
//  recorte, el combiner de texturas (GL_COMBINE), parte del estencil y las consultas de estado de los arreglos. Las
//  funciones de aca son VACIAS para que el backend compile; el backend no las usa en la Dreamcast porque declara esas
//  capacidades como ausentes (VBOSoportado / PlanosRecorteMax / MezclaTexturasSoportada / TieneTexGen = no) y el
//  editor toma el camino de siempre para eso (arreglos en RAM, varias pasadas, reflejo por software).
// ============================================================================
#ifdef W3D_DREAMCAST
#include <GL/gl.h>
#include <GL/glext.h>
#include <stddef.h>

#ifndef GL_DITHER
#define GL_DITHER               0x0BD0
#endif
#ifndef GL_VIEWPORT
#define GL_VIEWPORT             0x0BA2
#endif
#ifndef GL_MAX_CLIP_PLANES
#define GL_MAX_CLIP_PLANES      0x0D32
#endif
#ifndef GL_S
#define GL_S                    0x2000
#define GL_T                    0x2001
#endif
#ifndef GL_TEXTURE_GEN_MODE
#define GL_TEXTURE_GEN_MODE     0x2500
#endif
#ifndef GL_SPHERE_MAP
#define GL_SPHERE_MAP           0x2402
#endif
#ifndef GL_TEXTURE_GEN_S
#define GL_TEXTURE_GEN_S        0x0C60
#define GL_TEXTURE_GEN_T        0x0C61
#endif
#ifndef GL_POINT_SPRITE
#define GL_POINT_SPRITE         0x8861
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER         0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_VERTEX_ARRAY_POINTER
#define GL_VERTEX_ARRAY_SIZE            0x807A
#define GL_VERTEX_ARRAY_TYPE            0x807B
#define GL_VERTEX_ARRAY_STRIDE          0x807C
#define GL_NORMAL_ARRAY_TYPE            0x807E
#define GL_NORMAL_ARRAY_STRIDE          0x807F
#define GL_COLOR_ARRAY_SIZE             0x8081
#define GL_COLOR_ARRAY_TYPE             0x8082
#define GL_COLOR_ARRAY_STRIDE           0x8083
#define GL_TEXTURE_COORD_ARRAY_SIZE     0x8088
#define GL_TEXTURE_COORD_ARRAY_TYPE     0x8089
#define GL_TEXTURE_COORD_ARRAY_STRIDE   0x808A
#define GL_VERTEX_ARRAY_POINTER         0x808E
#define GL_NORMAL_ARRAY_POINTER         0x808F
#define GL_COLOR_ARRAY_POINTER          0x8090
#define GL_TEXTURE_COORD_ARRAY_POINTER  0x8092
#endif

// buffer objects: no hay (VBOSoportado() = false en la Dreamcast)
static inline void glGenBuffers(GLsizei, GLuint* b) { if (b) *b = 0; }
static inline void glDeleteBuffers(GLsizei, const GLuint*) {}
static inline void glBindBuffer(GLenum, GLuint) {}
static inline void glBufferData(GLenum, ptrdiff_t, const void*, GLenum) {}
// lo demas que el backend nombra pero en la Dreamcast no usa (o que la PVR no puede hacer)
static inline void glBlendEquation(GLenum) {}
static inline void glClearStencil(GLint) {}
static inline void glStencilMask(GLuint) {}
static inline void glClipPlane(GLenum, const double*) {}
static inline void glTexEnvfv(GLenum, GLenum, const GLfloat*) {}
static inline void glTexGeni(GLenum, GLenum, GLint) {}
static inline void glGetPointerv(GLenum, GLvoid** p) { if (p) *p = 0; }
#endif // W3D_DREAMCAST
#endif // W3D_GLDC_H
