#include "w3dGraphics.h" // abstraccion de graficos (independencia de OpenGL)
#include "Empty.h"
#include "objects/RenderColors.h" // gRenderColors / RC_selActive (color de seleccion, igual que el resto)
#include "objects/Textures.h"     // la IMAGEN de la referencia (Textures, TextureRetain / TextureRelease)
#ifdef W3D_SYMBIAN
#include <GLES/gl.h>
#else
#include <GL/gl.h>
#endif
#include <math.h>
#include <vector>

extern bool g_showEmpty; // toggle del submenu "Objects" (overlays)

bool Empty::DrawsSomething() { return g_showEmpty; }

// ---------------------------------------------------------------------------------------------------------------
//  El DIBUJO (lineas sueltas, de a pares). Se arma en el espacio de la PANTALLA (Z arriba, como Propiedades): el
//  circulo y el cuadrado quedan acostados en el piso. Despues se gira (rotDrawing), se escala (drawingSize) y se pasa
//  al interno (Y arriba). Solo la vista: el objeto no cambia.
// ---------------------------------------------------------------------------------------------------------------
static void Line(std::vector<GLfloat>& v, float ax, float ay, float az, float bx, float by, float bz) {
    v.push_back(ax); v.push_back(ay); v.push_back(az);
    v.push_back(bx); v.push_back(by); v.push_back(bz);
}
// CUANTAS LINEAS tiene un circulo. En una pantalla chica (el N95, la PSP, la 3DS, la Dreamcast) no se ve la diferencia
// y son menos vertices para dibujar: la mitad. La cruz, el cuadrado y el cubo ya son lo minimo (pedido del duenio).
#if defined(W3D_SYMBIAN) || defined(W3D_PSP) || defined(W3D_3DS) || defined(W3D_DREAMCAST)
static const int kSegCircle = 16, kSegSphere = 12;   // (la esfera son tres circulos)
#else
static const int kSegCircle = 32, kSegSphere = 24;
#endif
// un circulo de radio 0.5 en el plano de los ejes a y b (0 = X, 1 = Y, 2 = Z de la pantalla), con N lineas
static void Circle(std::vector<GLfloat>& v, int a, int b, int N) {
    for (int i = 0; i < N; i++) {
        const float t0 = 6.2831853f * (float)i / (float)N, t1 = 6.2831853f * (float)(i + 1) / (float)N;
        float p0[3] = { 0, 0, 0 }, p1[3] = { 0, 0, 0 };
        p0[a] = 0.5f * cosf(t0); p0[b] = 0.5f * sinf(t0);
        p1[a] = 0.5f * cosf(t1); p1[b] = 0.5f * sinf(t1);
        Line(v, p0[0], p0[1], p0[2], p1[0], p1[1], p1[2]);
    }
}
static void BuildShape(std::vector<GLfloat>& v, int shape) {
    v.clear();
    if (shape == EmptyCross) {
        Line(v, -0.5f, 0, 0, 0.5f, 0, 0); Line(v, 0, -0.5f, 0, 0, 0.5f, 0); Line(v, 0, 0, -0.5f, 0, 0, 0.5f);
    } else if (shape == EmptySquare) {   // acostado en el piso
        Line(v, -0.5f, -0.5f, 0, 0.5f, -0.5f, 0); Line(v, 0.5f, -0.5f, 0, 0.5f, 0.5f, 0);
        Line(v, 0.5f, 0.5f, 0, -0.5f, 0.5f, 0);   Line(v, -0.5f, 0.5f, 0, -0.5f, -0.5f, 0);
    } else if (shape == EmptyCube) {       // las 12 aristas
        const float h = 0.5f;
        for (int i = 0; i < 4; i++) {
            const float ax = (i == 0 || i == 3) ? -h : h, ay = (i < 2) ? -h : h;
            const float bx = (i == 0 || i == 1) ? h : -h, by = (i == 0 || i == 3) ? -h : h;
            Line(v, ax, ay, -h, bx, by, -h);   // abajo
            Line(v, ax, ay, h, bx, by, h);     // arriba
            Line(v, ax, ay, -h, ax, ay, h);    // los costados
        }
    } else if (shape == EmptySphere) {     // tres circulos, uno por plano
        Circle(v, 0, 1, kSegSphere); Circle(v, 0, 2, kSegSphere); Circle(v, 1, 2, kSegSphere);
    } else {                               // circulo, acostado en el piso
        Circle(v, 0, 1, kSegCircle);
    }
}

// girar (X, despues Y, despues Z, en grados, en la pantalla), escalar y pasar al interno (Y arriba). El MISMO camino para
// las lineas de las formas y para las esquinas de la imagen
static void ToInternal(std::vector<GLfloat>& v, const Vector3& rot, float t) {
    const float k = 0.0174532925f;
    const float cx = cosf(rot.x * k), sx = sinf(rot.x * k);
    const float cy = cosf(rot.y * k), sy = sinf(rot.y * k);
    const float cz = cosf(rot.z * k), sz = sinf(rot.z * k);
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        float x = v[i], y = v[i + 1], z = v[i + 2], q;
        q = y * cx - z * sx; z = y * sx + z * cx; y = q;      // X
        q = x * cy + z * sy; z = -x * sy + z * cy; x = q;     // Y
        q = x * cz - y * sz; y = x * sz + y * cz; x = q;      // Z
        v[i] = x * t; v[i + 1] = z * t; v[i + 2] = y * t;     // Z arriba -> Y arriba
    }
}

// ---------------------------------------------------------------------------------------------------------------
//  LA IMAGEN (forma EmptyImage): la referencia para modelar encima
// ---------------------------------------------------------------------------------------------------------------
Empty::~Empty() {
    Texture* t = ImageAlive();
    if (t) TextureRelease(t);
    image = NULL;
}
Texture* Empty::ImageAlive() const {
    if (!image) return NULL;
    for (size_t i = 0; i < Textures.size(); i++) if (Textures[i] == image) return image;   // (solo compara el puntero)
    return NULL;
}
void Empty::SetImage(Texture* t, int w, int h) {
    if (t) TextureRetain(t);   // (antes de soltar la de antes: puede ser la misma)
    Texture* prev = ImageAlive();
    image = t; imageW = w; imageH = h;
    if (prev) TextureRelease(prev);
}
void Empty::CopyDrawing(const Empty* o) {
    shape = o->shape; drawingSize = o->drawingSize; rotDrawing = o->rotDrawing;
    imageOpacity = o->imageOpacity;
    SetImage(o->ImageAlive(), o->imageW, o->imageH);   // (la copia retiene la suya)
}
void Empty::ImageHalfSize(float& hw, float& hh) const {
    int w = imageW, h = imageH;
    const Texture* t = ImageAlive();
    if ((w <= 0 || h <= 0) && t) { w = t->width; h = t->height; }
    float aw = 1.0f, ah = 1.0f;   // (sin imagen, o sin saber su tamanio: cuadrada)
    if (w > 0 && h > 0) { if (w >= h) ah = (float)h / (float)w; else aw = (float)w / (float)h; }
    const float s = (drawingSize > 0.0f) ? drawingSize : 1.0f;
    hw = 0.5f * aw * s; hh = 0.5f * ah * s;
}
void Empty::ImageCorners(float* out) const {
    float hw, hh; ImageHalfSize(hw, hh);
    const float s = (drawingSize > 0.0f) ? drawingSize : 1.0f;
    hw /= s; hh /= s;   // (ToInternal escala)
    // ACOSTADA en el plano X-Y del objeto, como el "Image Empty" de Blender: con rotacion (0, 0, 0) mira para arriba, y
    // Add > Reference la crea girada 90 grados en X -parada, mirando a la vista de frente, derecha y sin espejar-.
    // (Pedido del duenio, 2026-11-27: "a 0, 0, 0 de rotacion tendrian que estar como en 90 en X"; antes se dibujaba
    // parada con el objeto sin girar.) El ARRIBA de la imagen va hacia -Y (ejes del panel): el giro de 90 en X lo lleva
    // a +Z, arriba
    static std::vector<GLfloat> v(12);
    const float q[12] = { -hw, hh, 0,   hw, hh, 0,   -hw, -hh, 0,   hw, -hh, 0 };
    for (int i = 0; i < 12; i++) v[(size_t)i] = q[i];
    ToInternal(v, rotDrawing, s);
    for (int i = 0; i < 12; i++) out[i] = v[(size_t)i];
}
// el MARCO de la imagen (o de donde va, sin imagen): las 4 esquinas en lineas
static void ImageFrame(const float* c) {
    static GLfloat L[24];
    const int e[8] = { 0, 1, 1, 3, 3, 2, 2, 0 };
    for (int i = 0; i < 8; i++) for (int k = 0; k < 3; k++) L[i * 3 + k] = c[e[i] * 3 + k];
    w3dEngine::VertexPointer3f(0, L);
    w3dEngine::TexCoordPointer2f(12, L);   // (ver la nota del dibujo de las formas)
    w3dEngine::DrawLines(8);
}
static void RenderImage(const Empty* o) {
    float c[12]; o->ImageCorners(c);
    Texture* t = o->ImageAlive();
    const GLboolean lightWas = w3dEngine::IsEnabled(w3dEngine::Lighting);
    const GLboolean cullWas = w3dEngine::IsEnabled(w3dEngine::CullFace);
    const GLboolean texWas = w3dEngine::IsEnabled(w3dEngine::Texture2D);
    w3dEngine::Disable(w3dEngine::Lighting);
    w3dEngine::DisableArray(w3dEngine::NormalArray);
    w3dEngine::DisableArray(w3dEngine::ColorArray);
    w3dEngine::Disable(w3dEngine::CullFace);   // (se ve de los dos lados)
    if (t && t->iID) {
        // la imagen tal cual (sin luz): lo que se calca tiene que verse fiel
        static const GLfloat uv[8] = { 0, 1,  1, 1,  0, 0,  1, 0 };
        static const MeshIndex idx[6] = { 0, 1, 2,  2, 1, 3 };
        float a = o->imageOpacity; if (a < 0.0f) a = 0.0f; if (a > 1.0f) a = 1.0f;
        w3dEngine::Enable(w3dEngine::Texture2D);
        w3dEngine::BindTexture(t->iID);
        w3dEngine::EnableArray(w3dEngine::TexCoordArray);
        w3dEngine::TexCoordPointer2f(0, uv);
        w3dEngine::Enable(w3dEngine::Blend); w3dEngine::BlendAlpha();
        w3dEngine::DepthMask(a >= 0.999f);   // (transparente: no tapa lo de atras)
        w3dEngine::Color4f(1, 1, 1, a);
        w3dEngine::VertexPointer3f(0, c);
        w3dEngine::DrawTriangles(6, idx);
        w3dEngine::DepthMask(true);
        w3dEngine::Disable(w3dEngine::Blend);
    }
    // el MARCO: elegida (en el color de la seleccion) o sin imagen (gris, para ver donde va)
    if (o->select || !t || !t->iID) {
        w3dEngine::Disable(w3dEngine::Texture2D);
        if (o->select) { const float* k = gRenderColors[(o == ActiveObj) ? RC_selActive : RC_selInactive]; w3dEngine::Color4f(k[0], k[1], k[2], 1.0f); }
        else w3dEngine::Color4f(0.85f, 0.85f, 0.85f, 1.0f);
        ImageFrame(c);
    }
    if (texWas) w3dEngine::Enable(w3dEngine::Texture2D); else w3dEngine::Disable(w3dEngine::Texture2D);
    if (cullWas) w3dEngine::Enable(w3dEngine::CullFace);
    w3dEngine::EnableArray(w3dEngine::NormalArray);
    if (lightWas) w3dEngine::Enable(w3dEngine::Lighting);
}

void Empty::RenderObject() {
    if (!g_showEmpty) return; // oculto por el toggle "Empty" del menu de overlays
    if (shape == EmptyImage) { RenderImage(this); return; }
    static std::vector<GLfloat> v;
    BuildShape(v, shape);
    ToInternal(v, rotDrawing, (drawingSize > 0.0f) ? drawingSize : 1.0f);
    GLboolean lightWas = w3dEngine::IsEnabled(w3dEngine::Lighting);
    w3dEngine::Disable(w3dEngine::Lighting);
    w3dEngine::DisableArray(w3dEngine::NormalArray);
    // color de seleccion igual que el resto de los objetos: ACTIVO = verde (selActive); seleccionado no-activo =
    // verde secundario (selInactive); sin seleccionar = gris.
    if (select){
        const float* c = gRenderColors[(this == ActiveObj) ? RC_selActive : RC_selInactive];
        w3dEngine::Color4f(c[0], c[1], c[2], 1.0f);
    } else {
        w3dEngine::Color4f(0.85f, 0.85f, 0.85f, 1.0f);
    }
    w3dEngine::VertexPointer3f(0, &v[0]);
    // (las coordenadas de textura, a los MISMOS datos: con el arreglo prendido el driver las lee aunque no haya textura,
    //  y si no se fijan quedan las del que dibujo antes -el Hitbox: su arreglo de 24 vertices- y se leia de mas)
    w3dEngine::TexCoordPointer2f(12, &v[0]);
    w3dEngine::DrawLines((int)(v.size() / 3));
    w3dEngine::EnableArray(w3dEngine::NormalArray);
    if (lightWas) w3dEngine::Enable(w3dEngine::Lighting);
}
