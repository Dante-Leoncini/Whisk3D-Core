#ifndef W3D_GL_DESVIO_H
#define W3D_GL_DESVIO_H
// ============================================================================
//  w3dGLDesvio.h - lo que suben las texturas por GL directo (gfx/w3dTexture.cpp, el video) cuando el backend activo es
//  VULKAN: no hay contexto de GL, asi que cada glFoo de esos archivos pasa a vkglFoo (vk/w3dVkGL.cpp), el GL hecho con
//  Vulkan que usa ese backend. Con OpenGL / OpenGL ES 2.0 sigue yendo al driver. Va DESPUES de <GL/gl.h> (y glu.h).
//  Solo con varios backends en el ejecutable y Vulkan compilado (la PC); en el resto no hace nada.
// ============================================================================
#if defined(W3D_MULTIBACKEND) && defined(W3D_VULKAN)
#include "../vk/w3dVkGLApi.h"
#include "w3dBackend.h"
#define W3D_DESVIO_(fn, args) (w3dEngine::BackendActivo() == w3dEngine::BackendVulkan ? vkgl##fn args : (gl##fn) args)
#define glGenTextures(a, b) W3D_DESVIO_(GenTextures, (a, b))
#define glDeleteTextures(a, b) W3D_DESVIO_(DeleteTextures, (a, b))
#define glBindTexture(a, b) W3D_DESVIO_(BindTexture, (a, b))
#define glTexImage2D(a, b, c, d, e, f, g, h, i) W3D_DESVIO_(TexImage2D, (a, b, c, d, e, f, g, h, i))
#define glTexSubImage2D(a, b, c, d, e, f, g, h, i) W3D_DESVIO_(TexSubImage2D, (a, b, c, d, e, f, g, h, i))
#define glTexParameteri(a, b, c) W3D_DESVIO_(TexParameteri, (a, b, c))
#define glTexParameterf(a, b, c) W3D_DESVIO_(TexParameterf, (a, b, c))
#define glPixelStorei(a, b) W3D_DESVIO_(PixelStorei, (a, b))
#define glReadPixels(a, b, c, d, e, f, g) W3D_DESVIO_(ReadPixels, (a, b, c, d, e, f, g))
#define glCopyTexSubImage2D(a, b, c, d, e, f, g, h) W3D_DESVIO_(CopyTexSubImage2D, (a, b, c, d, e, f, g, h))
#define glGetString(a) W3D_DESVIO_(GetString, (a))
#define glGetFloatv(a, b) W3D_DESVIO_(GetFloatv, (a, b))
#define glGetIntegerv(a, b) W3D_DESVIO_(GetIntegerv, (a, b))
// (la piramide de mipmaps de escritorio: con Vulkan, el nivel 0 y glGenerateMipmap)
#define gluBuild2DMipmaps(t, interno, w, h, fmt, tipo, datos) \
    (w3dEngine::BackendActivo() == w3dEngine::BackendVulkan \
        ? (vkglTexImage2D(t, 0, interno, w, h, 0, fmt, tipo, datos), vkglGenerateMipmap(t), 0) \
        : (gluBuild2DMipmaps)(t, interno, w, h, fmt, tipo, datos))
#endif
#endif
