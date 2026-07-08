#pragma once

// ===============================
// Windows
// ===============================

// OJO: en Symbian NUNCA incluir windows.h, aunque el build/indexer defina _WIN32 (ej WINSCW o la config del indexer de Carbide). 
//El !defined(W3D_SYMBIAN) lo evita. En PC (sin W3D_SYMBIAN) queda IGUAL que antes.
#if defined(_WIN32) && !defined(W3D_SYMBIAN)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#endif

// ===============================
// Math defines
// ===============================

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ===============================
// Indice del index buffer de mallas (Mesh::faces)
// ===============================
// N95 = GLES 1.1: glDrawElements SOLO soporta GL_UNSIGNED_BYTE/SHORT -> indices de 16 bits (max 65535 verts/malla).
// PC / Android / WebGL / Nokia N8 = 32 bits (GL_UNSIGNED_INT) -> modelos grandes (200k+ verts). El importador
// RECHAZA en el N95 cualquier malla que pase los 16 bits;
// usamos unsigned short/int para no usar GLushort/GLuint y evitar la dependencia de OpenGL
#if defined(W3D_SYMBIAN) // || defined(__ANDROID__) // Descomentar si se requiere 1.1 despues
typedef unsigned short MeshIndex;
#else
typedef unsigned int   MeshIndex;
#endif
#define W3D_MAX_INDEX16 65535
