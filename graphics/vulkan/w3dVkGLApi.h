#ifndef W3D_VKGL_API_H
#define W3D_VKGL_API_H
// ============================================================================
//  w3dVkGLApi.h - las funciones de OpenGL ES 2.0 que hace graphics/vulkan/w3dVkGL.cpp con Vulkan (vkglFoo = glFoo). Van despues de
//  <GL/gl.h> (los tipos GLenum, GLuint...); aca estan los enums de GL 2.0 / ES 2.0 que el gl.h 1.1 de Windows no trae.
// ============================================================================
#include <stddef.h>

// (Android: los headers de ES ya los traen, en 32 bits con otro tipo de base: el de ES 1.1 GLsizeiptr, el de 2.0 los dos)
#ifndef GL_ES_VERSION_2_0
typedef char GLchar;
#if !defined(GL_VERSION_ES_CM_1_0)
typedef ptrdiff_t GLsizeiptr;
#endif
#endif
// (Android: su GL/gl.h es el de OpenGL ES 1.1, que no trae los enteros de 32 bits de GL 1.1)
#ifndef GL_INT
    #define GL_INT                  0x1404
#endif
#ifndef GL_UNSIGNED_INT
    #define GL_UNSIGNED_INT         0x1405
#endif
#ifndef GL_FRAGMENT_SHADER
    #define GL_FRAGMENT_SHADER      0x8B30
    #define GL_VERTEX_SHADER        0x8B31
    #define GL_COMPILE_STATUS       0x8B81
    #define GL_LINK_STATUS          0x8B82
    #define GL_INFO_LOG_LENGTH      0x8B84
#endif
#ifndef GL_ARRAY_BUFFER
    #define GL_ARRAY_BUFFER         0x8892
    #define GL_ELEMENT_ARRAY_BUFFER 0x8893
    #define GL_STREAM_DRAW          0x88E0
    #define GL_STATIC_DRAW          0x88E4
    #define GL_DYNAMIC_DRAW         0x88E8
#endif
#ifndef GL_TEXTURE0
    #define GL_TEXTURE0             0x84C0
#endif
#ifndef GL_TEXTURE1
    #define GL_TEXTURE1             0x84C1
#endif
#ifndef GL_CLAMP_TO_EDGE
    #define GL_CLAMP_TO_EDGE        0x812F
#endif
#ifndef GL_MIRRORED_REPEAT
    #define GL_MIRRORED_REPEAT      0x8370
#endif
#ifndef GL_FUNC_ADD
    #define GL_FUNC_ADD              0x8006
    #define GL_MIN                   0x8007
    #define GL_MAX                   0x8008
    #define GL_FUNC_SUBTRACT         0x800A
    #define GL_FUNC_REVERSE_SUBTRACT 0x800B
#endif
#ifndef GL_FRAMEBUFFER
    #define GL_FRAMEBUFFER          0x8D40
    #define GL_FRAMEBUFFER_BINDING  0x8CA6
    #define GL_COLOR_ATTACHMENT0    0x8CE0
    #define GL_DEPTH_ATTACHMENT     0x8D00
    #define GL_STENCIL_ATTACHMENT   0x8D20
    #define GL_FRAMEBUFFER_COMPLETE 0x8CD5
    #define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT 0x8CD6
    #define GL_FRAMEBUFFER_UNSUPPORTED 0x8CDD
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
    #define GL_UNSIGNED_SHORT_5_6_5   0x8363
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4
    #define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
    #define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#endif
#ifndef GL_BGRA
    #define GL_BGRA                 0x80E1
#endif
#ifndef GL_GENERATE_MIPMAP
    #define GL_GENERATE_MIPMAP      0x8191
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
    #define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
    #define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
#ifndef GL_INCR_WRAP
    #define GL_INCR_WRAP            0x8507
    #define GL_DECR_WRAP            0x8508
#endif
#ifndef GL_VERTEX_PROGRAM_POINT_SIZE
    #define GL_VERTEX_PROGRAM_POINT_SIZE 0x8642
#endif
#ifndef GL_POINT_SPRITE
    #define GL_POINT_SPRITE         0x8861
#endif

// ---- estado ----
void vkglEnable(GLenum cap);
void vkglDisable(GLenum cap);
GLboolean vkglIsEnabled(GLenum cap);
void vkglBlendFunc(GLenum src, GLenum dst);
void vkglBlendEquation(GLenum modo);
void vkglDepthFunc(GLenum f);
void vkglDepthMask(GLboolean w);
void vkglColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void vkglStencilMask(GLuint m);
void vkglStencilFunc(GLenum f, GLint ref, GLuint mask);
void vkglStencilOp(GLenum sfail, GLenum zfail, GLenum zpass);
void vkglClearStencil(GLint s);
void vkglClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void vkglClear(GLbitfield mask);
void vkglScissor(GLint x, GLint y, GLsizei w, GLsizei h);
void vkglViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void vkglPolygonOffset(GLfloat factor, GLfloat units);
void vkglLineWidth(GLfloat w);
void vkglFrontFace(GLenum modo);
void vkglCullFace(GLenum modo);
void vkglDepthRangef(GLfloat n, GLfloat f);
void vkglPixelStorei(GLenum p, GLint v);
void vkglFlush();
void vkglFinish();
GLenum vkglGetError();
void vkglGetIntegerv(GLenum p, GLint* v);
void vkglGetFloatv(GLenum p, GLfloat* v);
const GLubyte* vkglGetString(GLenum p);
// ---- buffers / atributos / dibujo ----
void vkglGenBuffers(GLsizei n, GLuint* b);
void vkglDeleteBuffers(GLsizei n, const GLuint* b);
void vkglBindBuffer(GLenum target, GLuint b);
void vkglBufferData(GLenum target, GLsizeiptr tam, const void* datos, GLenum uso);
void vkglVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* p);
void vkglEnableVertexAttribArray(GLuint i);
void vkglDisableVertexAttribArray(GLuint i);
void vkglVertexAttrib4fv(GLuint i, const GLfloat* v);
void vkglDrawArrays(GLenum modo, GLint first, GLsizei count);
void vkglDrawElements(GLenum modo, GLsizei count, GLenum type, const void* indices);
// ---- shaders ----
GLuint vkglCreateShader(GLenum tipo);
void vkglShaderSource(GLuint s, GLsizei n, const GLchar* const* partes, const GLint* largos);
void vkglCompileShader(GLuint s);
void vkglGetShaderiv(GLuint s, GLenum p, GLint* v);
void vkglGetShaderInfoLog(GLuint s, GLsizei max, GLsizei* largo, GLchar* log);
void vkglDeleteShader(GLuint s);
GLuint vkglCreateProgram();
void vkglAttachShader(GLuint p, GLuint s);
void vkglBindAttribLocation(GLuint p, GLuint i, const GLchar* nombre);
void vkglLinkProgram(GLuint p);
void vkglGetProgramiv(GLuint p, GLenum q, GLint* v);
void vkglGetProgramInfoLog(GLuint p, GLsizei max, GLsizei* largo, GLchar* log);
void vkglUseProgram(GLuint p);
void vkglDeleteProgram(GLuint p);
GLint vkglGetUniformLocation(GLuint p, const GLchar* nombre);
GLint vkglGetAttribLocation(GLuint p, const GLchar* nombre);
void vkglUniform1i(GLint loc, GLint v);
void vkglUniform1f(GLint loc, GLfloat v);
void vkglUniform4fv(GLint loc, GLsizei n, const GLfloat* v);
void vkglUniformMatrix3fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat* v);
void vkglUniformMatrix4fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat* v);
// ---- texturas ----
void vkglActiveTexture(GLenum unidad);
void vkglGenTextures(GLsizei n, GLuint* t);
void vkglDeleteTextures(GLsizei n, const GLuint* t);
void vkglBindTexture(GLenum target, GLuint t);
void vkglTexImage2D(GLenum target, GLint nivel, GLint interno, GLsizei w, GLsizei h, GLint borde, GLenum formato, GLenum tipo, const void* datos);
void vkglTexSubImage2D(GLenum target, GLint nivel, GLint x, GLint y, GLsizei w, GLsizei h, GLenum formato, GLenum tipo, const void* datos);
void vkglTexParameteri(GLenum target, GLenum p, GLint v);
void vkglTexParameterf(GLenum target, GLenum p, GLfloat v);
void vkglGenerateMipmap(GLenum target);
void vkglCopyTexSubImage2D(GLenum target, GLint nivel, GLint xoff, GLint yoff, GLint x, GLint y, GLsizei w, GLsizei h);
void vkglReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum formato, GLenum tipo, void* px);
// ---- framebuffers (render a una textura) ----
void vkglGenFramebuffers(GLsizei n, GLuint* f);
void vkglDeleteFramebuffers(GLsizei n, const GLuint* f);
void vkglBindFramebuffer(GLenum target, GLuint f);
void vkglFramebufferTexture2D(GLenum target, GLenum adjunto, GLenum textarget, GLuint t, GLint nivel);
GLenum vkglCheckFramebufferStatus(GLenum target);
#endif
