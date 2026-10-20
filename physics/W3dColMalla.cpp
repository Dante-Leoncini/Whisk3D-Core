// ============================================================================
//  W3dColMalla.cpp — ver W3dColMalla.h. Colision de personaje contra los
//  triangulos de una malla estatica (piso + paredes), con grilla XZ.
// ============================================================================
#include "physics/W3dColMalla.h"
#include "objects/Objects.h"
#include "objects/Mesh.h"
#include "math/Matrix4.h"
#include "script/W3dScript.h"   // W3dScriptParamObjeto
#include "base/w3dlog.h"
#include <math.h>
#include <vector>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace {

// min/max propios: fminf/fmaxf son C99 y no estan garantizados en el toolchain del N95
inline float Mn(float a, float b) { return a < b ? a : b; }
inline float Mx(float a, float b) { return a > b ? a : b; }

// un triangulo en MUNDO + su normal y su caja (para descartar rapido)
struct Tri {
    Vector3 a, b, c, n;
    float minX, maxX, minY, maxY, minZ, maxZ;
};

struct ColMalla {
    Object* obj;
    const void* firma;              // el buffer de vertices leido: si la malla se rearma, cambia -> se relee
    int nVerts, nIdx;
    std::vector<Tri> tris;
    // grilla XZ: celdas de 'celda' metros sobre la caja del mapa; CSR (offset por celda + indices)
    float x0, z0, celda;
    int nx, nz;
    std::vector<int> off, idx;
    // marca por triangulo para no probarlo dos veces en una consulta que toca varias celdas
    std::vector<unsigned> marca;
    unsigned consulta;
};

std::vector<ColMalla*> gMallas;

void Liberar(ColMalla* c) { delete c; }

// lee la malla a MUNDO y arma la grilla. NULL si no es una malla con triangulos.
ColMalla* Construir(Object* o) {
    if (!o || o->getType() != ObjectType::mesh) return NULL;
    Mesh* m = (Mesh*)o;
    if (!m->vertex || !m->faces || m->vertexSize <= 0 || m->facesSize < 3) return NULL;
    Matrix4 W; o->GetWorldMatrix(W);
    ColMalla* c = new ColMalla();
    c->obj = o; c->firma = m->vertex; c->nVerts = m->vertexSize; c->nIdx = m->facesSize;
    c->consulta = 0;
    float mnX = 1e30f, mxX = -1e30f, mnZ = 1e30f, mxZ = -1e30f;
    c->tris.reserve(m->facesSize / 3);
    for (int i = 0; i + 2 < m->facesSize; i += 3) {
        const int ia = m->faces[i], ib = m->faces[i + 1], ic = m->faces[i + 2];
        if (ia >= m->vertexSize || ib >= m->vertexSize || ic >= m->vertexSize) continue;
        Tri t;
        t.a = W * Vector3(m->vertex[ia * 3], m->vertex[ia * 3 + 1], m->vertex[ia * 3 + 2]);
        t.b = W * Vector3(m->vertex[ib * 3], m->vertex[ib * 3 + 1], m->vertex[ib * 3 + 2]);
        t.c = W * Vector3(m->vertex[ic * 3], m->vertex[ic * 3 + 1], m->vertex[ic * 3 + 2]);
        Vector3 n = Vector3::Cross(t.b - t.a, t.c - t.a);
        const float l = n.Length();
        if (l < 1e-9f) continue;                     // degenerado
        t.n = n * (1.0f / l);
        t.minX = Mn(t.a.x, Mn(t.b.x, t.c.x)); t.maxX = Mx(t.a.x, Mx(t.b.x, t.c.x));
        t.minY = Mn(t.a.y, Mn(t.b.y, t.c.y)); t.maxY = Mx(t.a.y, Mx(t.b.y, t.c.y));
        t.minZ = Mn(t.a.z, Mn(t.b.z, t.c.z)); t.maxZ = Mx(t.a.z, Mx(t.b.z, t.c.z));
        if (t.minX < mnX) mnX = t.minX; if (t.maxX > mxX) mxX = t.maxX;
        if (t.minZ < mnZ) mnZ = t.minZ; if (t.maxZ > mxZ) mxZ = t.maxZ;
        c->tris.push_back(t);
    }
    if (c->tris.empty()) { delete c; return NULL; }
    // celda: ~ que haya del orden de 64x64 celdas como mucho, y no menos de 2 m
    const float ancho = Mx(mxX - mnX, mxZ - mnZ);
    c->celda = Mx(2.0f, ancho / 64.0f);
    c->x0 = mnX; c->z0 = mnZ;
    c->nx = (int)((mxX - mnX) / c->celda) + 1;
    c->nz = (int)((mxZ - mnZ) / c->celda) + 1;
    const int nCeldas = c->nx * c->nz;
    std::vector<int> cuenta(nCeldas + 1, 0);
    for (int pasada = 0; pasada < 2; pasada++) {
        std::vector<int> pos;
        if (pasada == 1) {
            c->off.assign(nCeldas + 1, 0);
            for (int k = 0; k < nCeldas; k++) c->off[k + 1] = c->off[k] + cuenta[k];
            c->idx.assign(c->off[nCeldas], 0);
            pos.assign(c->off.begin(), c->off.end() - 1);
        }
        for (size_t t = 0; t < c->tris.size(); t++) {
            const Tri& T = c->tris[t];
            int cx0 = (int)((T.minX - c->x0) / c->celda), cx1 = (int)((T.maxX - c->x0) / c->celda);
            int cz0 = (int)((T.minZ - c->z0) / c->celda), cz1 = (int)((T.maxZ - c->z0) / c->celda);
            if (cx1 >= c->nx) cx1 = c->nx - 1; if (cz1 >= c->nz) cz1 = c->nz - 1;
            for (int cz = cz0; cz <= cz1; cz++)
                for (int cx = cx0; cx <= cx1; cx++) {
                    const int k = cz * c->nx + cx;
                    if (pasada == 0) cuenta[k]++;
                    else c->idx[pos[k]++] = (int)t;
                }
        }
    }
    c->marca.assign(c->tris.size(), 0);
    w3dLogf("colision: '%s' %d triangulos, grilla %dx%d de %.1f m", o->name.c_str(), (int)c->tris.size(),
            c->nx, c->nz, c->celda);
    return c;
}

ColMalla* Buscar(Object* o) {
    if (!o) return NULL;
    for (size_t i = 0; i < gMallas.size(); i++) {
        ColMalla* c = gMallas[i];
        if (c->obj != o) continue;
        Mesh* m = (Mesh*)o;   // la malla se rearmo (edicion, recarga): releer
        if (o->getType() == ObjectType::mesh && (m->vertex != c->firma || m->vertexSize != c->nVerts || m->facesSize != c->nIdx)) {
            Liberar(c);
            gMallas.erase(gMallas.begin() + i);
            break;
        }
        return c;
    }
    ColMalla* c = Construir(o);
    if (c) gMallas.push_back(c);
    return c;
}

// recorre los triangulos de las celdas que cubre la caja XZ [x0,x1]x[z0,z1] (una vez cada uno)
template <class F>
void PorTriangulo(ColMalla* c, float ax, float az, float bx, float bz, F& f) {
    int cx0 = (int)floorf((ax - c->x0) / c->celda), cx1 = (int)floorf((bx - c->x0) / c->celda);
    int cz0 = (int)floorf((az - c->z0) / c->celda), cz1 = (int)floorf((bz - c->z0) / c->celda);
    if (cx0 < 0) cx0 = 0; if (cz0 < 0) cz0 = 0;
    if (cx1 >= c->nx) cx1 = c->nx - 1; if (cz1 >= c->nz) cz1 = c->nz - 1;
    if (cx0 > cx1 || cz0 > cz1) return;
    if (++c->consulta == 0) { c->marca.assign(c->marca.size(), 0); c->consulta = 1; }
    for (int cz = cz0; cz <= cz1; cz++)
        for (int cx = cx0; cx <= cx1; cx++) {
            const int k = cz * c->nx + cx;
            for (int q = c->off[k]; q < c->off[k + 1]; q++) {
                const int t = c->idx[q];
                if (c->marca[t] == c->consulta) continue;
                c->marca[t] = c->consulta;
                f(c->tris[t]);
            }
        }
}

// punto del triangulo mas cercano a p (Ericson, Real-Time Collision Detection 5.1.5)
Vector3 MasCercano(const Vector3& p, const Tri& t) {
    const Vector3 ab = t.b - t.a, ac = t.c - t.a, ap = p - t.a;
    const float d1 = ab.Dot(ap), d2 = ac.Dot(ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return t.a;
    const Vector3 bp = p - t.b;
    const float d3 = ab.Dot(bp), d4 = ac.Dot(bp);
    if (d3 >= 0.0f && d4 <= d3) return t.b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return t.a + ab * (d1 / (d1 - d3));
    const Vector3 cp = p - t.c;
    const float d5 = ab.Dot(cp), d6 = ac.Dot(cp);
    if (d6 >= 0.0f && d5 <= d6) return t.c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return t.a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        return t.b + (t.c - t.b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float den = 1.0f / (va + vb + vc);
    return t.a + ab * (vb * den) + ac * (vc * den);
}

// --- consultas (functores: C++03 sin lambdas) ---
struct FSuelo {
    float x, z, techo, mejor; bool hay;
    void operator()(const Tri& t) {
        if (x < t.minX || x > t.maxX || z < t.minZ || z > t.maxZ) return;
        if (fabsf(t.n.y) < 1e-4f) return;               // vertical: no es piso
        // baricentricas en XZ
        const float v0x = t.b.x - t.a.x, v0z = t.b.z - t.a.z, v1x = t.c.x - t.a.x, v1z = t.c.z - t.a.z;
        const float px = x - t.a.x, pz = z - t.a.z;
        const float den = v0x * v1z - v1x * v0z;
        if (fabsf(den) < 1e-12f) return;
        const float u = (px * v1z - v1x * pz) / den, v = (v0x * pz - px * v0z) / den;
        const float e = -1e-5f;
        if (u < e || v < e || u + v > 1.0f - e) return;
        const float y = t.a.y + (t.b.y - t.a.y) * u + (t.c.y - t.a.y) * v;
        if (y > techo) return;
        if (!hay || y > mejor) { mejor = y; hay = true; }
    }
};

struct FPared {
    Vector3 centro; float radio; Vector3 empuje; bool toco;
    void operator()(const Tri& t) {
        if (centro.y + radio < t.minY || centro.y - radio > t.maxY) return;
        const Vector3 q = MasCercano(centro, t);
        Vector3 d = centro - q;
        d.y = 0.0f;                                    // solo en horizontal: no levanta ni hunde
        const float dy = centro.y - q.y;
        const float r2 = radio * radio - dy * dy;      // radio del circulo a la altura del punto
        if (r2 <= 0.0f) return;
        const float r = sqrtf(r2);
        float l = d.Length();
        if (l >= r) return;
        if (l < 1e-5f) {                                // centro sobre el plano: sale por la normal
            d = Vector3(t.n.x, 0.0f, t.n.z);
            l = d.Length();
            if (l < 1e-5f) return;
            d = d * (1.0f / l);
            l = 0.0f;
        } else d = d * (1.0f / l);
        const float p = r - l;
        // se acumula el mayor empuje por eje (dos paredes en esquina empujan las dos)
        const Vector3 e = d * p;
        if (fabsf(e.x) > fabsf(empuje.x)) empuje.x = e.x;
        if (fabsf(e.z) > fabsf(empuje.z)) empuje.z = e.z;
        toco = true;
    }
};

struct FRayo {
    Vector3 o, d; float mejor; Vector3 n; bool hay;
    void operator()(const Tri& t) {                   // Moller-Trumbore, las dos caras
        const Vector3 e1 = t.b - t.a, e2 = t.c - t.a;
        const Vector3 p = Vector3::Cross(d, e2);
        const float det = e1.Dot(p);
        if (fabsf(det) < 1e-9f) return;
        const float inv = 1.0f / det;
        const Vector3 s = o - t.a;
        const float u = s.Dot(p) * inv;
        if (u < 0.0f || u > 1.0f) return;
        const Vector3 q = Vector3::Cross(s, e1);
        const float v = d.Dot(q) * inv;
        if (v < 0.0f || u + v > 1.0f) return;
        const float dist = e2.Dot(q) * inv;
        if (dist < 0.0f || dist >= mejor) return;
        mejor = dist; n = t.n; hay = true;
    }
};

} // namespace

bool W3dColSuelo(Object* malla, float x, float y, float z, float subida, float* sueloY) {
    ColMalla* c = Buscar(malla);
    if (!c) return false;
    FSuelo f; f.x = x; f.z = z; f.techo = y + subida; f.mejor = 0.0f; f.hay = false;
    PorTriangulo(c, x, z, x, z, f);
    if (f.hay && sueloY) *sueloY = f.mejor;
    return f.hay;
}

bool W3dColPared(Object* malla, float* x, float y, float* z, float radio, float alto) {
    ColMalla* c = Buscar(malla);
    if (!c || !x || !z || radio <= 0.0f) return false;
    bool toco = false;
    // esferas apiladas de las rodillas a la cabeza; 3 iteraciones resuelven esquinas
    const float y0 = y + radio, y1 = y + (alto > radio * 2.0f ? alto - radio : radio);
    int nEsf = (int)((y1 - y0) / radio) + 1; if (nEsf < 1) nEsf = 1; if (nEsf > 6) nEsf = 6;
    for (int it = 0; it < 3; it++) {
        FPared f; f.radio = radio; f.empuje = Vector3(0, 0, 0); f.toco = false;
        for (int k = 0; k < nEsf; k++) {
            f.centro = Vector3(*x, nEsf > 1 ? y0 + (y1 - y0) * k / (nEsf - 1) : y0, *z);
            PorTriangulo(c, *x - radio, *z - radio, *x + radio, *z + radio, f);
        }
        if (!f.toco) break;
        *x += f.empuje.x; *z += f.empuje.z;
        toco = true;
    }
    return toco;
}

float W3dColRayo(Object* malla, const Vector3& o, const Vector3& d, float max, Vector3* n) {
    ColMalla* c = Buscar(malla);
    if (!c || max <= 0.0f) return -1.0f;
    const Vector3 fin = o + d * max;
    FRayo f; f.o = o; f.d = d; f.mejor = max; f.hay = false;
    PorTriangulo(c, Mn(o.x, fin.x), Mn(o.z, fin.z), Mx(o.x, fin.x), Mx(o.z, fin.z), f);
    if (!f.hay) return -1.0f;
    if (n) *n = f.n;
    return f.mejor;
}

void W3dColOlvidar(Object* malla) {
    for (size_t i = 0; i < gMallas.size(); i++)
        if (gMallas[i]->obj == malla) { Liberar(gMallas[i]); gMallas.erase(gMallas.begin() + i); return; }
}

void W3dColLimpiar(void) {
    for (size_t i = 0; i < gMallas.size(); i++) Liberar(gMallas[i]);
    gMallas.clear();
}

// ---------------------------------------------------------------------------
//  binds lua
// ---------------------------------------------------------------------------
static int LColSuelo(lua_State* L) {
    Object* o = W3dScriptParamObjeto((void*)L, 1);
    const float x = (float)luaL_checknumber(L, 2), y = (float)luaL_checknumber(L, 3), z = (float)luaL_checknumber(L, 4);
    const float sub = (float)luaL_optnumber(L, 5, 0.5);
    float s = 0.0f;
    if (!W3dColSuelo(o, x, y, z, sub, &s)) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, s);
    return 1;
}

static int LColPared(lua_State* L) {
    Object* o = W3dScriptParamObjeto((void*)L, 1);
    float x = (float)luaL_checknumber(L, 2);
    const float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    const float r = (float)luaL_checknumber(L, 5);
    const float alto = (float)luaL_optnumber(L, 6, 1.7);
    W3dColPared(o, &x, y, &z, r, alto);
    lua_pushnumber(L, x);
    lua_pushnumber(L, z);
    return 2;
}

static int LColRayo(lua_State* L) {
    Object* o = W3dScriptParamObjeto((void*)L, 1);
    Vector3 org((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4));
    Vector3 d((float)luaL_checknumber(L, 5), (float)luaL_checknumber(L, 6), (float)luaL_checknumber(L, 7));
    const float mx = (float)luaL_checknumber(L, 8);
    if (d.Length() < 1e-9f) { lua_pushnil(L); return 1; }
    d = d.Normalized();
    Vector3 n;
    const float t = W3dColRayo(o, org, d, mx, &n);
    if (t < 0.0f) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, t); lua_pushnumber(L, n.x); lua_pushnumber(L, n.y); lua_pushnumber(L, n.z);
    return 4;
}

// ---------------------------------------------------------------------------
//  RAYO CONTRA MALLAS QUE SE MUEVEN (rayoMalla): sin cache ni grilla, con la
//  pose de ESTE momento (matriz de mundo actual, vertices skinneados si la malla
//  tiene armature). Para lo chico que se anima: el cuerpo de un enemigo, un
//  barril, una granada. Recorre el objeto y sus descendientes VISIBLES y
//  devuelve el impacto mas cercano. Las dos caras cuentan (la bala no mira la
//  normal). El parametro t es el del rayo de MUNDO aunque se pruebe en local
//  (la matriz es afin: org + t*dir pasa a lo + t*ld con el mismo t).
// ---------------------------------------------------------------------------
static bool RayoTriangulo(const Vector3& o, const Vector3& d, const Vector3& a, const Vector3& b,
                          const Vector3& c, float* t) {
    const Vector3 e1 = b - a, e2 = c - a;
    const Vector3 p = Vector3::Cross(d, e2);
    const float det = e1.Dot(p);
    if (det > -1e-12f && det < 1e-12f) return false;
    const float inv = 1.0f / det;
    const Vector3 s = o - a;
    const float u = s.Dot(p) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const Vector3 q = Vector3::Cross(s, e1);
    const float v = d.Dot(q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    *t = e2.Dot(q) * inv;
    return *t >= 0.0f;
}

static void RayoObjeto(Object* o, const Vector3& org, const Vector3& dir, float* mejor, Object** quien, Vector3* nrm) {
    if (!o || !o->visible) return;
    if (o->getType() == ObjectType::mesh) {
        Mesh* m = (Mesh*)o;
        // la pose de AHORA: si el render no la skinneo (malla culleada, o un tick sin dibujo) se skinnea aca; SkinearMesh
        // esta cacheado por pose, asi que si ya esta al dia no cuesta nada
        if (m->skinArmature) { extern void SkinearMesh(Mesh*); SkinearMesh(m); }
        const GLfloat* src = (m->skinArmature && m->skinVertex && m->skinShapeSerial != 0xFFFFFFFFu) ? m->skinVertex : m->vertex;
        if (src && m->faces && m->vertexSize > 0 && m->facesSize >= 3) {
            Matrix4 W, Wi; o->GetWorldMatrix(W);
            if (Matrix4::InvertirAfin(W, Wi)) {
                const Vector3 lo = Wi * org, ld = (Wi * (org + dir)) - lo;
                for (int i = 0; i + 2 < m->facesSize; i += 3) {
                    const int ia = m->faces[i], ib = m->faces[i + 1], ic = m->faces[i + 2];
                    if (ia >= m->vertexSize || ib >= m->vertexSize || ic >= m->vertexSize) continue;
                    const Vector3 a(src[ia * 3], src[ia * 3 + 1], src[ia * 3 + 2]);
                    const Vector3 b(src[ib * 3], src[ib * 3 + 1], src[ib * 3 + 2]);
                    const Vector3 c(src[ic * 3], src[ic * 3 + 1], src[ic * 3 + 2]);
                    float t;
                    if (RayoTriangulo(lo, ld, a, b, c, &t) && t < *mejor) {
                        *mejor = t; *quien = o;
                        // la normal a mundo (sin la escala: alcanza para orientar un impacto)
                        const Vector3 n = Vector3::Cross(b - a, c - a);
                        Vector3 nw(W.m[0] * n.x + W.m[4] * n.y + W.m[8] * n.z,
                                   W.m[1] * n.x + W.m[5] * n.y + W.m[9] * n.z,
                                   W.m[2] * n.x + W.m[6] * n.y + W.m[10] * n.z);
                        const float l = nw.Length();
                        *nrm = l > 1e-12f ? nw * (1.0f / l) : Vector3(0, 1, 0);
                    }
                }
            }
        }
    }
    for (size_t k = 0; k < o->Childrens.size(); k++) RayoObjeto(o->Childrens[k], org, dir, mejor, quien, nrm);
}

// rayoMalla(obj, ox, oy, oz, dx, dy, dz [, max]) -> t, objeto, nx, ny, nz  (o nil si no toca nada a menos de max)
static int LRayoMalla(lua_State* L) {
    Object* o = W3dScriptParamObjeto((void*)L, 1);
    Vector3 org((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4));
    Vector3 d((float)luaL_checknumber(L, 5), (float)luaL_checknumber(L, 6), (float)luaL_checknumber(L, 7));
    float mejor = (float)luaL_optnumber(L, 8, 1e30);
    if (!o || d.Length() < 1e-9f) { lua_pushnil(L); return 1; }
    d = d.Normalized();
    Object* quien = NULL; Vector3 n(0, 1, 0);
    RayoObjeto(o, org, d, &mejor, &quien, &n);
    if (!quien) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, mejor);
    lua_pushlightuserdata(L, quien);
    lua_pushnumber(L, n.x); lua_pushnumber(L, n.y); lua_pushnumber(L, n.z);
    return 5;
}

static int LColOlvidar(lua_State* L) {
    W3dColOlvidar(W3dScriptParamObjeto((void*)L, 1));
    return 0;
}

void W3dColMallaRegistrarBinds(void* Lv) {
    lua_State* L = (lua_State*)Lv;
    // un VM nuevo = una corrida nueva: lo leido antes puede apuntar a objetos que ya no existen
    W3dColLimpiar();
    lua_pushcfunction(L, LColSuelo);   lua_setglobal(L, "colSuelo");
    lua_pushcfunction(L, LColPared);   lua_setglobal(L, "colPared");
    lua_pushcfunction(L, LColRayo);    lua_setglobal(L, "colRayo");
    lua_pushcfunction(L, LColOlvidar); lua_setglobal(L, "colOlvidar");
    lua_pushcfunction(L, LRayoMalla);  lua_setglobal(L, "rayoMalla");
}
