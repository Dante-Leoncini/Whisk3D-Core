// ============================================================================
//  W3dColMesh.cpp — ver W3dColMesh.h. Colision de personaje contra los
//  triangulos de una malla estatica (piso + paredes), con grilla XZ.
// ============================================================================
#include "physics/W3dColMesh.h"
#include "objects/Objects.h"
#include "objects/Mesh.h"
#include "math/Matrix4.h"
#include "script/W3dScript.h"   // W3dScriptParamObject
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

struct ColMesh {
    Object* obj;
    const void* signature;              // el buffer de vertices leido: si la malla se rearma, cambia -> se relee
    int nVerts, nIdx;
    std::vector<Tri> tris;
    // grilla XZ: celdas de 'celda' metros sobre la caja del mapa; CSR (offset por celda + indices)
    float x0, z0, cell;
    int nx, nz;
    Vector3 mn, mx;                 // la caja de todos los triangulos
    std::vector<int> off, idx;
    // marca por triangulo para no probarlo dos veces en una consulta que toca varias celdas
    std::vector<unsigned> mark;
    unsigned query;
};

std::vector<ColMesh*> gMeshes;

void Free(ColMesh* c) { delete c; }

// lee la malla a MUNDO y arma la grilla. NULL si no es una malla con triangulos.
ColMesh* Build(Object* o) {
    if (!o || o->getType() != ObjectType::mesh) return NULL;
    Mesh* m = (Mesh*)o;
    if (!m->vertex || !m->faces || m->vertexSize <= 0 || m->facesSize < 3) return NULL;
    Matrix4 W; o->GetWorldMatrix(W);
    ColMesh* c = new ColMesh();
    c->obj = o; c->signature = m->vertex; c->nVerts = m->vertexSize; c->nIdx = m->facesSize;
    c->query = 0;
    float mnX = 1e30f, mxX = -1e30f, mnY = 1e30f, mxY = -1e30f, mnZ = 1e30f, mxZ = -1e30f;
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
        if (t.minY < mnY) mnY = t.minY; if (t.maxY > mxY) mxY = t.maxY;
        if (t.minZ < mnZ) mnZ = t.minZ; if (t.maxZ > mxZ) mxZ = t.maxZ;
        c->tris.push_back(t);
    }
    if (c->tris.empty()) { delete c; return NULL; }
    c->mn = Vector3(mnX, mnY, mnZ); c->mx = Vector3(mxX, mxY, mxZ);
    // celda: ~ que haya del orden de 64x64 celdas como mucho, y no menos de 2 m
    const float width = Mx(mxX - mnX, mxZ - mnZ);
    c->cell = Mx(2.0f, width / 64.0f);
    c->x0 = mnX; c->z0 = mnZ;
    c->nx = (int)((mxX - mnX) / c->cell) + 1;
    c->nz = (int)((mxZ - mnZ) / c->cell) + 1;
    const int nCells = c->nx * c->nz;
    std::vector<int> count(nCells + 1, 0);
    for (int pass = 0; pass < 2; pass++) {
        std::vector<int> pos;
        if (pass == 1) {
            c->off.assign(nCells + 1, 0);
            for (int k = 0; k < nCells; k++) c->off[k + 1] = c->off[k] + count[k];
            c->idx.assign(c->off[nCells], 0);
            pos.assign(c->off.begin(), c->off.end() - 1);
        }
        for (size_t t = 0; t < c->tris.size(); t++) {
            const Tri& T = c->tris[t];
            int cx0 = (int)((T.minX - c->x0) / c->cell), cx1 = (int)((T.maxX - c->x0) / c->cell);
            int cz0 = (int)((T.minZ - c->z0) / c->cell), cz1 = (int)((T.maxZ - c->z0) / c->cell);
            if (cx1 >= c->nx) cx1 = c->nx - 1; if (cz1 >= c->nz) cz1 = c->nz - 1;
            for (int cz = cz0; cz <= cz1; cz++)
                for (int cx = cx0; cx <= cx1; cx++) {
                    const int k = cz * c->nx + cx;
                    if (pass == 0) count[k]++;
                    else c->idx[pos[k]++] = (int)t;
                }
        }
    }
    c->mark.assign(c->tris.size(), 0);
    w3dLogf("colision: '%s' %d triangulos, grilla %dx%d de %.1f m", o->name.c_str(), (int)c->tris.size(),
            c->nx, c->nz, c->cell);
    return c;
}

ColMesh* Find(Object* o) {
    if (!o) return NULL;
    for (size_t i = 0; i < gMeshes.size(); i++) {
        ColMesh* c = gMeshes[i];
        if (c->obj != o) continue;
        Mesh* m = (Mesh*)o;   // la malla se rearmo (edicion, recarga): releer
        if (o->getType() == ObjectType::mesh && (m->vertex != c->signature || m->vertexSize != c->nVerts || m->facesSize != c->nIdx)) {
            Free(c);
            gMeshes.erase(gMeshes.begin() + i);
            break;
        }
        return c;
    }
    ColMesh* c = Build(o);
    if (c) gMeshes.push_back(c);
    return c;
}

// recorre los triangulos de las celdas que cubre la caja XZ [x0,x1]x[z0,z1] (una vez cada uno)
template <class F>
void ByTriangle(ColMesh* c, float ax, float az, float bx, float bz, F& f) {
    int cx0 = (int)floorf((ax - c->x0) / c->cell), cx1 = (int)floorf((bx - c->x0) / c->cell);
    int cz0 = (int)floorf((az - c->z0) / c->cell), cz1 = (int)floorf((bz - c->z0) / c->cell);
    if (cx0 < 0) cx0 = 0; if (cz0 < 0) cz0 = 0;
    if (cx1 >= c->nx) cx1 = c->nx - 1; if (cz1 >= c->nz) cz1 = c->nz - 1;
    if (cx0 > cx1 || cz0 > cz1) return;
    if (++c->query == 0) { c->mark.assign(c->mark.size(), 0); c->query = 1; }
    for (int cz = cz0; cz <= cz1; cz++)
        for (int cx = cx0; cx <= cx1; cx++) {
            const int k = cz * c->nx + cx;
            for (int q = c->off[k]; q < c->off[k + 1]; q++) {
                const int t = c->idx[q];
                if (c->mark[t] == c->query) continue;
                c->mark[t] = c->query;
                f(c->tris[t]);
            }
        }
}

// punto del triangulo mas cercano a p (Ericson, Real-Time Collision Detection 5.1.5)
Vector3 Nearest(const Vector3& p, const Tri& t) {
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
struct FGround {
    float x, z, ceiling, best; bool has;
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
        if (y > ceiling) return;
        if (!has || y > best) { best = y; has = true; }
    }
};

struct FWall {
    Vector3 center; float radius; Vector3 push; bool touched;
    void operator()(const Tri& t) {
        if (center.y + radius < t.minY || center.y - radius > t.maxY) return;
        const Vector3 q = Nearest(center, t);
        Vector3 d = center - q;
        d.y = 0.0f;                                    // solo en horizontal: no levanta ni hunde
        const float dy = center.y - q.y;
        const float r2 = radius * radius - dy * dy;      // radio del circulo a la altura del punto
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
        if (fabsf(e.x) > fabsf(push.x)) push.x = e.x;
        if (fabsf(e.z) > fabsf(push.z)) push.z = e.z;
        touched = true;
    }
};

struct FRay {
    Vector3 o, d; float best; Vector3 n; bool has;
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
        if (dist < 0.0f || dist >= best) return;
        best = dist; n = t.n; has = true;
    }
};

struct FCollect {
    Vector3 mn, mx; W3dColTri* out; int max, n;
    void operator()(const Tri& t) {
        if (n >= max) return;
        if (t.maxX < mn.x || t.minX > mx.x || t.maxY < mn.y || t.minY > mx.y || t.maxZ < mn.z || t.minZ > mx.z) return;
        W3dColTri& o = out[n++];
        o.a = t.a; o.b = t.b; o.c = t.c; o.n = t.n;
    }
};

} // namespace

bool W3dColBox(Object* mesh, Vector3* mn, Vector3* mx) {
    ColMesh* c = Find(mesh);
    if (!c) return false;
    if (mn) *mn = c->mn;
    if (mx) *mx = c->mx;
    return true;
}

int W3dColTriangles(Object* mesh, const Vector3& mn, const Vector3& mx, W3dColTri* out, int max) {
    ColMesh* c = Find(mesh);
    if (!c || !out || max <= 0) return 0;
    FCollect f; f.mn = mn; f.mx = mx; f.out = out; f.max = max; f.n = 0;
    ByTriangle(c, mn.x, mn.z, mx.x, mx.z, f);
    return f.n;
}

bool W3dColGround(Object* mesh, float x, float y, float z, float upload, float* groundY) {
    ColMesh* c = Find(mesh);
    if (!c) return false;
    FGround f; f.x = x; f.z = z; f.ceiling = y + upload; f.best = 0.0f; f.has = false;
    ByTriangle(c, x, z, x, z, f);
    if (f.has && groundY) *groundY = f.best;
    return f.has;
}

bool W3dColWall(Object* mesh, float* x, float y, float* z, float radius, float height) {
    ColMesh* c = Find(mesh);
    if (!c || !x || !z || radius <= 0.0f) return false;
    bool touched = false;
    // esferas apiladas de las rodillas a la cabeza; 3 iteraciones resuelven esquinas
    const float y0 = y + radius, y1 = y + (height > radius * 2.0f ? height - radius : radius);
    int nSphere = (int)((y1 - y0) / radius) + 1; if (nSphere < 1) nSphere = 1; if (nSphere > 6) nSphere = 6;
    for (int it = 0; it < 3; it++) {
        FWall f; f.radius = radius; f.push = Vector3(0, 0, 0); f.touched = false;
        for (int k = 0; k < nSphere; k++) {
            f.center = Vector3(*x, nSphere > 1 ? y0 + (y1 - y0) * k / (nSphere - 1) : y0, *z);
            ByTriangle(c, *x - radius, *z - radius, *x + radius, *z + radius, f);
        }
        if (!f.touched) break;
        *x += f.push.x; *z += f.push.z;
        touched = true;
    }
    return touched;
}

float W3dColRay(Object* mesh, const Vector3& o, const Vector3& d, float max, Vector3* n) {
    ColMesh* c = Find(mesh);
    if (!c || max <= 0.0f) return -1.0f;
    const Vector3 end = o + d * max;
    FRay f; f.o = o; f.d = d; f.best = max; f.has = false;
    ByTriangle(c, Mn(o.x, end.x), Mn(o.z, end.z), Mx(o.x, end.x), Mx(o.z, end.z), f);
    if (!f.has) return -1.0f;
    if (n) *n = f.n;
    return f.best;
}

void W3dColForget(Object* mesh) {
    for (size_t i = 0; i < gMeshes.size(); i++)
        if (gMeshes[i]->obj == mesh) { Free(gMeshes[i]); gMeshes.erase(gMeshes.begin() + i); return; }
}

void W3dColClear(void) {
    for (size_t i = 0; i < gMeshes.size(); i++) Free(gMeshes[i]);
    gMeshes.clear();
}

// ---------------------------------------------------------------------------
//  binds lua
//  Lua usa Z ARRIBA (los ejes del panel): un (x,y,z) de lua es el (x,z,y) del motor. Aca adentro todo sigue en los
//  ejes del motor (Y arriba, grilla XZ); la conversion se hace en la frontera con W3dLuaAxes (ver W3dScript.h).
// ---------------------------------------------------------------------------
// un vector 3D de lua (3 numeros desde 'idx') -> en los ejes del motor
static Vector3 LuaVec3(lua_State* L, int idx) {
    float x = (float)luaL_checknumber(L, idx), y = (float)luaL_checknumber(L, idx + 1), z = (float)luaL_checknumber(L, idx + 2);
    W3dLuaAxes(y, z);
    return Vector3(x, y, z);
}
// un vector 3D del motor -> 3 numeros a lua, en sus ejes
static void PushVec3(lua_State* L, Vector3 v) {
    W3dLuaAxes(v.y, v.z);
    lua_pushnumber(L, v.x); lua_pushnumber(L, v.y); lua_pushnumber(L, v.z);
}

// colGround(malla, x, y, z [, subida]) -> la ALTURA del piso (la z de lua) debajo del punto, o nil
static int LColGround(lua_State* L) {
    Object* o = W3dScriptParamObject((void*)L, 1);
    const Vector3 p = LuaVec3(L, 2);
    const float sub = (float)luaL_optnumber(L, 5, 0.5);
    float s = 0.0f;
    if (!W3dColGround(o, p.x, p.y, p.z, sub, &s)) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, s);   // una altura: la y del motor es la z de lua, el numero es el mismo
    return 1;
}

// colWall(malla, x, y, z, radio [, alto]) -> x, y: el punto empujado afuera de las paredes, en el plano HORIZONTAL de
// lua (XY; en el motor es el XZ). La z (la altura, los pies) no cambia y no se devuelve
static int LColWall(lua_State* L) {
    Object* o = W3dScriptParamObject((void*)L, 1);
    Vector3 p = LuaVec3(L, 2);
    const float r = (float)luaL_checknumber(L, 5);
    const float height = (float)luaL_optnumber(L, 6, 1.7);
    W3dColWall(o, &p.x, p.y, &p.z, r, height);
    lua_pushnumber(L, p.x);
    lua_pushnumber(L, p.z);   // la z del motor es la y de lua
    return 2;
}

// colRay(malla, ox,oy,oz, dx,dy,dz, max) -> t, nx,ny,nz | nil   (origen, direccion y normal en ejes de lua)
static int LColRay(lua_State* L) {
    Object* o = W3dScriptParamObject((void*)L, 1);
    Vector3 org = LuaVec3(L, 2);
    Vector3 d = LuaVec3(L, 5);
    const float mx = (float)luaL_checknumber(L, 8);
    if (d.Length() < 1e-9f) { lua_pushnil(L); return 1; }
    d = d.Normalized();
    Vector3 n;
    const float t = W3dColRay(o, org, d, mx, &n);
    if (t < 0.0f) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, t); PushVec3(L, n);
    return 4;
}

// ---------------------------------------------------------------------------
//  RAYO CONTRA MALLAS QUE SE MUEVEN (rayMesh): sin cache ni grilla, con la
//  pose de ESTE momento (matriz de mundo actual, vertices skinneados si la malla
//  tiene armature). Para lo chico que se anima: el cuerpo de un enemigo, un
//  barril, una granada. Recorre el objeto y sus descendientes VISIBLES y
//  devuelve el impacto mas cercano. Las dos caras cuentan (la bala no mira la
//  normal). El parametro t es el del rayo de MUNDO aunque se pruebe en local
//  (la matriz es afin: org + t*dir pasa a lo + t*ld con el mismo t).
// ---------------------------------------------------------------------------
static bool RayTriangle(const Vector3& o, const Vector3& d, const Vector3& a, const Vector3& b,
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

static void RayObject(Object* o, const Vector3& org, const Vector3& dir, float* best, Object** who, Vector3* nrm) {
    if (!o || !o->visible) return;
    if (o->getType() == ObjectType::mesh) {
        Mesh* m = (Mesh*)o;
        // la pose de AHORA: si el render no la skinneo (malla culleada, o un tick sin dibujo) se skinnea aca; SkinMesh
        // esta cacheado por pose, asi que si ya esta al dia no cuesta nada
        if (m->skinArmature) { extern void SkinMesh(Mesh*); SkinMesh(m); }
        const GLfloat* src = (m->skinArmature && m->skinVertex && m->skinShapeSerial != 0xFFFFFFFFu) ? m->skinVertex : m->vertex;
        if (src && m->faces && m->vertexSize > 0 && m->facesSize >= 3) {
            Matrix4 W, Wi; o->GetWorldMatrix(W);
            if (Matrix4::InvertAffine(W, Wi)) {
                const Vector3 lo = Wi * org, ld = (Wi * (org + dir)) - lo;
                for (int i = 0; i + 2 < m->facesSize; i += 3) {
                    const int ia = m->faces[i], ib = m->faces[i + 1], ic = m->faces[i + 2];
                    if (ia >= m->vertexSize || ib >= m->vertexSize || ic >= m->vertexSize) continue;
                    const Vector3 a(src[ia * 3], src[ia * 3 + 1], src[ia * 3 + 2]);
                    const Vector3 b(src[ib * 3], src[ib * 3 + 1], src[ib * 3 + 2]);
                    const Vector3 c(src[ic * 3], src[ic * 3 + 1], src[ic * 3 + 2]);
                    float t;
                    if (RayTriangle(lo, ld, a, b, c, &t) && t < *best) {
                        *best = t; *who = o;
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
    for (size_t k = 0; k < o->Childrens.size(); k++) RayObject(o->Childrens[k], org, dir, best, who, nrm);
}

// rayMesh(obj, ox, oy, oz, dx, dy, dz [, max]) -> t, objeto, nx, ny, nz  (o nil si no toca nada a menos de max)
// (origen, direccion y normal en ejes de lua, Z arriba)
static int LRayMesh(lua_State* L) {
    Object* o = W3dScriptParamObject((void*)L, 1);
    Vector3 org = LuaVec3(L, 2);
    Vector3 d = LuaVec3(L, 5);
    float best = (float)luaL_optnumber(L, 8, 1e30);
    if (!o || d.Length() < 1e-9f) { lua_pushnil(L); return 1; }
    d = d.Normalized();
    Object* who = NULL; Vector3 n(0, 1, 0);
    RayObject(o, org, d, &best, &who, &n);
    if (!who) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, best);
    lua_pushlightuserdata(L, who);
    PushVec3(L, n);
    return 5;
}

static int LColForget(lua_State* L) {
    W3dColForget(W3dScriptParamObject((void*)L, 1));
    return 0;
}

void W3dColMeshRegisterBinds(void* Lv) {
    lua_State* L = (lua_State*)Lv;
    // un VM nuevo = una corrida nueva: lo leido antes puede apuntar a objetos que ya no existen
    W3dColClear();
    lua_pushcfunction(L, LColGround);   lua_setglobal(L, "colGround");
    lua_pushcfunction(L, LColWall);   lua_setglobal(L, "colWall");
    lua_pushcfunction(L, LColRay);    lua_setglobal(L, "colRay");
    lua_pushcfunction(L, LColForget); lua_setglobal(L, "colForget");
    lua_pushcfunction(L, LRayMesh);  lua_setglobal(L, "rayMesh");
}
