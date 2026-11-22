#ifndef W3D_GL_DEVIATION_H
#define W3D_GL_DEVIATION_H
// ============================================================================
//  w3dGLDeviation.h - lo que suben las texturas por GL directo (graphics/w3dTexture.cpp, el video) cuando el backend activo es
//  VULKAN: no hay contexto de GL, asi que cada glFoo de esos archivos pasa a vkglFoo (graphics/vulkan/w3dVkGL.cpp), el GL hecho con
//  Vulkan que usa ese backend. Con OpenGL / OpenGL ES 2.0 sigue yendo al driver. Va DESPUES de <GL/gl.h> (y glu.h).
//  Solo con varios backends en el ejecutable y Vulkan compilado (la PC); en el resto no hace nada.
// ============================================================================
#if defined(W3D_MULTIBACKEND) && defined(W3D_VULKAN)
#include "w3dVkGLApi.h"
#include "w3dBackend.h"
#define W3D_DEVIATION_(fn, args) (w3dEngine::ActiveBackend() == w3dEngine::BackendVulkan ? vkgl##fn args : (gl##fn) args)
#define glGenTextures(a, b) W3D_DEVIATION_(GenTextures, (a, b))
#define glDeleteTextures(a, b) W3D_DEVIATION_(DeleteTextures, (a, b))
#define glBindTexture(a, b) W3D_DEVIATION_(BindTexture, (a, b))
#define glTexImage2D(a, b, c, d, e, f, g, h, i) W3D_DEVIATION_(TexImage2D, (a, b, c, d, e, f, g, h, i))
#define glTexSubImage2D(a, b, c, d, e, f, g, h, i) W3D_DEVIATION_(TexSubImage2D, (a, b, c, d, e, f, g, h, i))
#define glTexParameteri(a, b, c) W3D_DEVIATION_(TexParameteri, (a, b, c))
#define glTexParameterf(a, b, c) W3D_DEVIATION_(TexParameterf, (a, b, c))
#define glPixelStorei(a, b) W3D_DEVIATION_(PixelStorei, (a, b))
#define glReadPixels(a, b, c, d, e, f, g) W3D_DEVIATION_(ReadPixels, (a, b, c, d, e, f, g))
#define glCopyTexSubImage2D(a, b, c, d, e, f, g, h) W3D_DEVIATION_(CopyTexSubImage2D, (a, b, c, d, e, f, g, h))
#define glGetString(a) W3D_DEVIATION_(GetString, (a))
#define glGetFloatv(a, b) W3D_DEVIATION_(GetFloatv, (a, b))
#define glGetIntegerv(a, b) W3D_DEVIATION_(GetIntegerv, (a, b))
// (la piramide de mipmaps de escritorio: con Vulkan, el nivel 0 y glGenerateMipmap)
#define gluBuild2DMipmaps(t, internal, w, h, fmt, type, data) \
    (w3dEngine::ActiveBackend() == w3dEngine::BackendVulkan \
        ? (vkglTexImage2D(t, 0, internal, w, h, 0, fmt, type, data), vkglGenerateMipmap(t), 0) \
        : (gluBuild2DMipmaps)(t, internal, w, h, fmt, type, data))
#endif
#endif
