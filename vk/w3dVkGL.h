#ifndef W3D_VKGL_H
#define W3D_VKGL_H
// ============================================================================
//  w3dVkGL.h - lo incluye el backend de OpenGL ES 2.0 (gles2/w3dGraphicsGLES2.cpp) cuando se compila como el de Vulkan
//  (vk/w3dGraphicsVk.cpp, W3D_VKGL): cada glFoo de ese archivo pasa a ser vkglFoo (vk/w3dVkGL.cpp). Los nombres de GL
//  quedan intactos en el resto del ejecutable (el backend OpenGL los sigue llamando de opengl32 / libGL).
// ============================================================================
#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif
#include <GL/gl.h>   // (los tipos y los enums de GL 1.1)
#include "w3dVkGLApi.h"

#define glEnable vkglEnable
#define glDisable vkglDisable
#define glIsEnabled vkglIsEnabled
#define glBlendFunc vkglBlendFunc
#define glBlendEquation vkglBlendEquation
#define glDepthFunc vkglDepthFunc
#define glDepthMask vkglDepthMask
#define glColorMask vkglColorMask
#define glStencilMask vkglStencilMask
#define glStencilFunc vkglStencilFunc
#define glStencilOp vkglStencilOp
#define glClearStencil vkglClearStencil
#define glClearColor vkglClearColor
#define glClear vkglClear
#define glScissor vkglScissor
#define glViewport vkglViewport
#define glPolygonOffset vkglPolygonOffset
#define glLineWidth vkglLineWidth
#define glFrontFace vkglFrontFace
#define glCullFace vkglCullFace
#define glDepthRangef vkglDepthRangef
#define glPixelStorei vkglPixelStorei
#define glFlush vkglFlush
#define glFinish vkglFinish
#define glGetError vkglGetError
#define glGetIntegerv vkglGetIntegerv
#define glGetFloatv vkglGetFloatv
#define glGetString vkglGetString
#define glGenBuffers vkglGenBuffers
#define glDeleteBuffers vkglDeleteBuffers
#define glBindBuffer vkglBindBuffer
#define glBufferData vkglBufferData
#define glVertexAttribPointer vkglVertexAttribPointer
#define glEnableVertexAttribArray vkglEnableVertexAttribArray
#define glDisableVertexAttribArray vkglDisableVertexAttribArray
#define glVertexAttrib4fv vkglVertexAttrib4fv
#define glDrawArrays vkglDrawArrays
#define glDrawElements vkglDrawElements
#define glCreateShader vkglCreateShader
#define glShaderSource vkglShaderSource
#define glCompileShader vkglCompileShader
#define glGetShaderiv vkglGetShaderiv
#define glGetShaderInfoLog vkglGetShaderInfoLog
#define glDeleteShader vkglDeleteShader
#define glCreateProgram vkglCreateProgram
#define glAttachShader vkglAttachShader
#define glBindAttribLocation vkglBindAttribLocation
#define glLinkProgram vkglLinkProgram
#define glGetProgramiv vkglGetProgramiv
#define glGetProgramInfoLog vkglGetProgramInfoLog
#define glUseProgram vkglUseProgram
#define glDeleteProgram vkglDeleteProgram
#define glGetUniformLocation vkglGetUniformLocation
#define glGetAttribLocation vkglGetAttribLocation
#define glUniform1i vkglUniform1i
#define glUniform1f vkglUniform1f
#define glUniform4fv vkglUniform4fv
#define glUniformMatrix3fv vkglUniformMatrix3fv
#define glUniformMatrix4fv vkglUniformMatrix4fv
#define glActiveTexture vkglActiveTexture
#define glGenTextures vkglGenTextures
#define glDeleteTextures vkglDeleteTextures
#define glBindTexture vkglBindTexture
#define glTexImage2D vkglTexImage2D
#define glTexSubImage2D vkglTexSubImage2D
#define glTexParameteri vkglTexParameteri
#define glTexParameterf vkglTexParameterf
#define glGenerateMipmap vkglGenerateMipmap
#define glCopyTexSubImage2D vkglCopyTexSubImage2D
#define glReadPixels vkglReadPixels
#define glGenFramebuffers vkglGenFramebuffers
#define glDeleteFramebuffers vkglDeleteFramebuffers
#define glBindFramebuffer vkglBindFramebuffer
#define glFramebufferTexture2D vkglFramebufferTexture2D
#define glCheckFramebufferStatus vkglCheckFramebufferStatus
#endif
