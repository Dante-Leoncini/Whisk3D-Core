// EL MODIFICADOR SCATTER: ver edit/ScatterMod.h (que hace) y edit/Modifier.h (sus campos)
#include "edit/ScatterMod.h"
#include "edit/Modifier.h"
#include "edit/PolyMesh.h"
#include "edit/MeshEdit.h"
#include "objects/Mesh.h"
#include "objects/Objects.h"
#include "objects/ProxyW3d.h"
#include "io/Prefabs.h"
#include "io/Libraries.h"
#include "W3dRoots.h"
#include "w3dGraphics.h"
#include "w3dlog.h"
#include <math.h>
#include <map>
#include <set>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------------------------------------------
//  EL AZAR: xorshift32 propio (rand() cambia entre plataformas y lo comparte todo el programa)
// ---------------------------------------------------------------------------------------------------------------
namespace {
struct ScatRng {
    unsigned s;
    explicit ScatRng(unsigned seed) : s((seed + 1u) * 2654435761u ^ 0x9E3779B9u) { if (!s) s = 1u; Next(); Next(); }
    unsigned Next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float F() { return (float)(Next() >> 8) * (1.0f / 16777216.0f); }   // [0, 1)
};

struct Sample { Vector3 p, n; };

const float kPi = 3.14159265358979f;

Vector3 Nrm(const Vector3& v, const Vector3& fallback) {
    const float l = v.Length();
    return l > 1e-12f ? v * (1.0f / l) : fallback;
}

// la normal de una cara por Newell (no la tuerce un quad alabeado); su largo es el DOBLE del area
Vector3 FaceNormal(const PolyMesh& W, size_t f) {
    Vector3 n(0, 0, 0);
    const std::vector<int>& F = W.F[f];
    const size_t k = F.size();
    for (size_t i = 0; i < k; i++) {
        const Vector3& a = W.P[(size_t)F[i]];
        const Vector3& b = W.P[(size_t)F[(i + 1) % k]];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return n;
}

// 'cuantos' indices distintos de 0..n-1 (todos, en orden, si alcanza): Fisher-Yates parcial
void Choose(int n, int count, ScatRng& r, std::vector<int>& out) {
    out.clear();
    if (n <= 0 || count <= 0) return;
    std::vector<int> idx((size_t)n);
    for (int i = 0; i < n; i++) idx[(size_t)i] = i;
    if (count >= n) { out.swap(idx); return; }
    for (int i = 0; i < count; i++) {
        const int j = i + (int)(r.Next() % (unsigned)(n - i));
        const int t = idx[(size_t)i]; idx[(size_t)i] = idx[(size_t)j]; idx[(size_t)j] = t;
        out.push_back(idx[(size_t)i]);
    }
}

// los LUGARES de las copias sobre W (espacio local del emisor), con su normal
void Samples(const PolyMesh& W, int emit, int count, unsigned seed, std::vector<Sample>& out) {
    out.clear();
    if (W.P.empty() || count <= 0) return;
    ScatRng r(seed);
    const Vector3 up(0, 1, 0);
    std::vector<Vector3> fn(W.F.size());
    for (size_t f = 0; f < W.F.size(); f++) fn[f] = FaceNormal(W, f);
    if (emit == ScatterEmit::Verts) {
        std::vector<Vector3> vn(W.P.size(), Vector3(0, 0, 0));
        std::vector<char> used(W.P.size(), 0);
        for (size_t f = 0; f < W.F.size(); f++)
            for (size_t c = 0; c < W.F[f].size(); c++) { vn[(size_t)W.F[f][c]] += fn[f]; used[(size_t)W.F[f][c]] = 1; }
        for (size_t e = 0; e < W.E.size(); e++) { used[(size_t)W.E[e].first] = 1; used[(size_t)W.E[e].second] = 1; }
        std::vector<int> live;
        for (size_t i = 0; i < W.P.size(); i++) if (used[i]) live.push_back((int)i);
        std::vector<int> pick; Choose((int)live.size(), count, r, pick);
        for (size_t i = 0; i < pick.size(); i++) {
            const int v = live[(size_t)pick[i]];
            Sample s; s.p = W.P[(size_t)v]; s.n = Nrm(vn[(size_t)v], up); out.push_back(s);
        }
    } else if (emit == ScatterEmit::FaceCenters) {
        std::vector<int> pick; Choose((int)W.F.size(), count, r, pick);
        for (size_t i = 0; i < pick.size(); i++) {
            const std::vector<int>& F = W.F[(size_t)pick[i]];
            Vector3 c(0, 0, 0);
            for (size_t k = 0; k < F.size(); k++) c += W.P[(size_t)F[k]];
            Sample s; s.p = c * (1.0f / (float)F.size()); s.n = Nrm(fn[(size_t)pick[i]], up); out.push_back(s);
        }
    } else if (emit == ScatterEmit::Edges) {
        // aristas unicas (de las caras + las sueltas), con la normal de sus caras; se elige parejo por LARGO
        std::map<std::pair<int, int>, Vector3> en;
        for (size_t f = 0; f < W.F.size(); f++) {
            const std::vector<int>& F = W.F[f];
            for (size_t k = 0; k < F.size(); k++) {
                int a = F[k], b = F[(k + 1) % F.size()];
                if (a == b) continue;
                if (a > b) { const int t = a; a = b; b = t; }
                en[std::make_pair(a, b)] += fn[f];
            }
        }
        for (size_t e = 0; e < W.E.size(); e++) {
            int a = W.E[e].first, b = W.E[e].second;
            if (a == b) continue;
            if (a > b) { const int t = a; a = b; b = t; }
            en.insert(std::make_pair(std::make_pair(a, b), Vector3(0, 0, 0)));
        }
        std::vector<std::pair<int, int> > E; std::vector<Vector3> N; std::vector<float> acc;
        float tot = 0.0f;
        for (std::map<std::pair<int, int>, Vector3>::const_iterator it = en.begin(); it != en.end(); ++it) {
            tot += (W.P[(size_t)it->first.second] - W.P[(size_t)it->first.first]).Length();
            E.push_back(it->first); N.push_back(it->second); acc.push_back(tot);
        }
        if (E.empty() || tot <= 0.0f) return;
        for (int i = 0; i < count; i++) {
            const float x = r.F() * tot, t = r.F();
            size_t lo = 0, hi = acc.size() - 1;
            while (lo < hi) { const size_t mid = (lo + hi) / 2; if (acc[mid] < x) lo = mid + 1; else hi = mid; }
            const Vector3& a = W.P[(size_t)E[lo].first];
            const Vector3& b = W.P[(size_t)E[lo].second];
            Sample s; s.p = a + (b - a) * t; s.n = Nrm(N[lo], up); out.push_back(s);
        }
    } else {   // Faces: cualquier punto, parejo por AREA (cada cara en abanico)
        std::vector<int> triF; std::vector<int> triA, triB, triC; std::vector<float> acc;
        float tot = 0.0f;
        for (size_t f = 0; f < W.F.size(); f++) {
            const std::vector<int>& F = W.F[f];
            for (size_t k = 1; k + 1 < F.size(); k++) {
                const Vector3& a = W.P[(size_t)F[0]];
                const Vector3& b = W.P[(size_t)F[k]];
                const Vector3& c = W.P[(size_t)F[k + 1]];
                const float ar = Vector3::Cross(b - a, c - a).Length() * 0.5f;
                if (ar <= 0.0f) continue;
                tot += ar;
                triF.push_back((int)f); triA.push_back(F[0]); triB.push_back(F[k]); triC.push_back(F[k + 1]);
                acc.push_back(tot);
            }
        }
        if (acc.empty() || tot <= 0.0f) return;
        for (int i = 0; i < count; i++) {
            const float x = r.F() * tot;
            float u = r.F(), v = r.F();
            if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; }
            size_t lo = 0, hi = acc.size() - 1;
            while (lo < hi) { const size_t mid = (lo + hi) / 2; if (acc[mid] < x) lo = mid + 1; else hi = mid; }
            const Vector3& a = W.P[(size_t)triA[lo]];
            const Vector3& b = W.P[(size_t)triB[lo]];
            const Vector3& c = W.P[(size_t)triC[lo]];
            Sample s; s.p = a + (b - a) * u + (c - a) * v; s.n = Nrm(fn[(size_t)triF[lo]], up); out.push_back(s);
        }
    }
}

// Rodrigues: 'v' girado 'ang' radianes alrededor del eje unitario 'k'
Vector3 Turn(const Vector3& v, const Vector3& k, float ang) {
    const float c = cosf(ang), s = sinf(ang);
    return v * c + Vector3::Cross(k, v) * s + k * (k.Dot(v) * (1.0f - c));
}

// la matriz de UNA copia: parada sobre 'up', girada, inclinada, escalada y corrida sobre su arriba
void CopyMatrix(const Vector3& pos, const Vector3& up, float spin, float tiltDir, float tilt, float scale, float offset,
                float* m) {
    const Vector3 ref = fabsf(up.z) < 0.9f ? Vector3(0, 0, 1) : Vector3(1, 0, 0);
    Vector3 X = Nrm(Vector3::Cross(up, ref), Vector3(1, 0, 0));
    Vector3 Y = up;
    Vector3 Z = Vector3::Cross(X, Y);
    // giro alrededor de su arriba
    const float c = cosf(spin), s = sinf(spin);
    const Vector3 X2 = X * c - Z * s, Z2 = X * s + Z * c;
    X = X2; Z = Z2;
    // inclinacion: alrededor de un eje horizontal (de la copia) elegido al azar
    if (tilt != 0.0f) {
        const Vector3 k = Nrm(X * cosf(tiltDir) + Z * sinf(tiltDir), X);
        X = Turn(X, k, tilt); Y = Turn(Y, k, tilt); Z = Turn(Z, k, tilt);
    }
    const Vector3 T = pos + up * offset;
    m[0] = X.x * scale; m[1] = X.y * scale; m[2] = X.z * scale; m[3] = 0.0f;
    m[4] = Y.x * scale; m[5] = Y.y * scale; m[6] = Y.z * scale; m[7] = 0.0f;
    m[8] = Z.x * scale; m[9] = Z.y * scale; m[10] = Z.z * scale; m[11] = 0.0f;
    m[12] = T.x; m[13] = T.y; m[14] = T.z; m[15] = 1.0f;
}

// 'o' esta adentro de 'anc' (o es 'anc')
bool Inside(const Object* o, const Object* anc) {
    for (const Object* p = o; p; p = p->Parent) if (p == anc) return true;
    return false;
}

bool HasMesh(Object* o) {
    if (!o) return false;
    if (o->getType() == ObjectType::mesh) return true;
    for (size_t i = 0; i < o->Childrens.size(); i++) if (HasMesh(o->Childrens[i])) return true;
    return false;
}

// los hijos de 'o' como fuentes: las colecciones se abren; lo que no tiene ninguna malla (luces, camaras) no va
void Units(Object* o, Mesh* emitter, std::vector<Object*>& out, int depth) {
    if (!o || depth > 32) return;
    for (size_t i = 0; i < o->Childrens.size(); i++) {
        Object* c = o->Childrens[i];
        if (!c || c == (Object*)emitter || Inside((Object*)emitter, c)) continue;
        // (las colecciones y la raiz de una escena -la de una libreria llega como un objeto Scene- se abren)
        if (c->getType() == ObjectType::collection || c->getType() == ObjectType::scene) Units(c, emitter, out, depth + 1);
        else if (HasMesh(c)) out.push_back(c);
    }
}

// los runtimes con una libreria cargada: se sueltan ANTES de cerrar las librerias del proyecto (W3dScatterForgetAll)
std::set<W3dScatterRuntime*>& WithProxy() { static std::set<W3dScatterRuntime*> s; return s; }
void FreeProxy(W3dScatterRuntime& rt) {
    if (!rt.libProxy) return;
    W3dPrefabRelease((InstancePrefab*)rt.libProxy);
    W3dFreeSubtree(rt.libProxy);
    rt.libProxy = NULL;
    rt.libKey.clear();
    WithProxy().erase(&rt);
}

// el .w3d externo: una ESCENA de una libreria, generada en un proxy fuera de la escena (sus hijos son las fuentes)
Object* LibProxy(Modifier* md) {
    W3dScatterRuntime& rt = md->scatRt;
    if (md->scatLib.empty()) return NULL;
    std::string scene = md->scatScene;
    if (scene.empty()) {
        std::vector<std::string> es; W3dLibsElements(md->scatLib, W3D_LIB_SCENE, es);
        if (!es.empty()) scene = es[0];
    }
    const std::string key = md->scatLib + "|" + scene;
    if (rt.libProxy && rt.libKey == key) return rt.libProxy;
    FreeProxy(rt);
    rt.libKey = key;
    if (scene.empty()) {
        w3dLogfW("[scatter] la libreria '%s' no tiene escenas (o no esta vinculada)", md->scatLib.c_str());
        return NULL;
    }
    // (generar toca la seleccion y el activo: se devuelven como estaban, la libreria se carga "por atras")
    Object* active = ActiveObj; const unsigned activeSerial = ActiveObj ? ActiveObj->serial : 0;
    const std::vector<Object*> sel = ObjSelects;
    // (nace AFUERA de la escena, como una raiz: sin padre un objeto se cuelga de SceneCollection -se dibujaria en el
    //  origen y se guardaria con el proyecto-)
    const bool rootCreating = W3dRootCreating;
    W3dRootCreating = true;
    ProxyW3d* px = new ProxyW3d(NULL, Vector3(0, 0, 0));
    W3dRootCreating = rootCreating;
    px->name = "Scatter:" + key;
    px->Set(md->scatLib, W3D_LIB_SCENE, scene);
    if (!W3dPrefabGenerate(px))
        w3dLogfW("[scatter] no se pudo cargar la escena '%s' de la libreria '%s'", scene.c_str(), md->scatLib.c_str());
    ActiveObj = W3dLiveObjectSerial(active, activeSerial) ? active : NULL;
    ObjSelects.clear();
    for (size_t i = 0; i < sel.size(); i++) if (W3dObjectLive(sel[i])) ObjSelects.push_back(sel[i]);
    rt.libProxy = px;
    WithProxy().insert(&rt);
    return px;
}

// las mallas de una fuente con su matriz RELATIVA a la fuente (la fuente misma: identidad)
void MeshesOf(Object* o, const Matrix4& rel, std::vector<std::pair<Mesh*, Matrix4> >& out, int depth) {
    if (!o || depth > 32) return;
    if (o->getType() == ObjectType::mesh) out.push_back(std::make_pair((Mesh*)o, rel));
    for (size_t i = 0; i < o->Childrens.size(); i++) {
        Object* c = o->Childrens[i];
        if (!c || !c->visible) continue;
        Matrix4 L; c->GetMatrixBase(L);
        MeshesOf(c, rel * L, out, depth + 1);
    }
}

float Det3(const Matrix4& M) {
    return M.m[0] * (M.m[5] * M.m[10] - M.m[9] * M.m[6]) - M.m[4] * (M.m[1] * M.m[10] - M.m[9] * M.m[2]) +
           M.m[8] * (M.m[1] * M.m[6] - M.m[5] * M.m[2]);
}

bool ScatterOn(const Mesh* m, const Modifier* md) {
    if (!md || md->type != ModifierType::Scatter || !md->showViewport) return false;
    if ((const Object*)m == g_editMesh && !md->showEdit) return false;
    return true;
}

int gScatterDepth = 0;   // una fuente con su propio Scatter: hasta 2 niveles (A reparte B que reparte A no cuelga)

}   // namespace

W3dScatterRuntime::~W3dScatterRuntime() { FreeProxy(*this); }

void W3dScatterForgetAll() {
    std::set<W3dScatterRuntime*> all = WithProxy();
    for (std::set<W3dScatterRuntime*>::iterator it = all.begin(); it != all.end(); ++it) FreeProxy(**it);
}

void W3dScatterPool(Modifier* md, Mesh* emitter, std::vector<Object*>& out) {
    out.clear();
    if (!md) return;
    switch (md->scatSource) {
        case ScatterSource::Obj:
            if (md->target && md->target != (Object*)emitter && !Inside((Object*)emitter, md->target) && HasMesh(md->target))
                out.push_back(md->target);
            break;
        case ScatterSource::Coll:
            if (md->target) {
                if (md->target->getType() == ObjectType::collection) Units(md->target, emitter, out, 0);
                else if (md->target != (Object*)emitter && !Inside((Object*)emitter, md->target) && HasMesh(md->target))
                    out.push_back(md->target);
            }
            break;
        case ScatterSource::Scene: {
            const int idx = W3dRootFind(W3D_ROOT_SCENE, md->scatScene);
            if (idx < 0) break;
            if (!W3dRoots()[(size_t)idx].root) W3dRootLoad(idx);
            Units(W3dRoots()[(size_t)idx].root, emitter, out, 0);
            break;
        }
        case ScatterSource::Lib: {
            // (la escena de la libreria se genera colgada de UNA raiz -un vacio "Scene"-: sus hijos son las fuentes)
            Object* px = LibProxy(md);
            if (px && px->Childrens.size() == 1 && px->Childrens[0] &&
                (px->Childrens[0]->getType() == ObjectType::empty || px->Childrens[0]->getType() == ObjectType::scene))
                px = px->Childrens[0];
            Units(px, emitter, out, 0);
            break;
        }
    }
}

void W3dScatterSourceMatrix(const float* copy16, Object* src, Matrix4& out) {
    Matrix4 C; for (int k = 0; k < 16; k++) C.m[k] = copy16[k];
    Matrix4 L; src->GetMatrixBase(L);
    L.m[12] = 0.0f; L.m[13] = 0.0f; L.m[14] = 0.0f;   // (su giro y su escala; su lugar no)
    out = C * L;
}

unsigned W3dScatterSignature(Modifier* md, Mesh* emitter) {
    std::vector<Object*> pool; W3dScatterPool(md, emitter, pool);
    unsigned h = 2166136261u;
    h = (h ^ (unsigned)pool.size()) * 16777619u;
    Matrix4 I; I.Identity();
    for (size_t i = 0; i < pool.size(); i++) {
        std::vector<std::pair<Mesh*, Matrix4> > ms; MeshesOf(pool[i], I, ms, 0);
        for (size_t k = 0; k < ms.size(); k++) {
            h = (h ^ (unsigned)(size_t)ms[k].first) * 16777619u;
            h = (h ^ ms[k].first->geoVersion) * 16777619u;
        }
    }
    return h ? h : 1u;
}

bool W3dScatterStale(Mesh* m) {
    if (!m) return false;
    for (size_t i = 0; i < m->modifiers.size(); i++) {
        Modifier* md = m->modifiers[i];
        if (!ScatterOn(m, md) || md->scatMode != 0) continue;   // (las instancias dibujan la fuente como esta)
        if (W3dScatterSignature(md, m) != md->scatRt.sig) return true;
    }
    return false;
}

void W3dScatterRun(Modifier* md, Mesh* emitter, PolyMesh& W, bool render) {
    (void)render;
    W3dScatterRuntime& rt = md->scatRt;
    rt.mats.clear(); rt.pick.clear();
    std::vector<Object*> pool; W3dScatterPool(md, emitter, pool);
    rt.sig = W3dScatterSignature(md, emitter);
    int count = (int)(md->scatCount + 0.5f);
    if (count < 0) count = 0;
    if (count > 1000000) count = 1000000;
    const unsigned seed = (unsigned)(int)(md->scatSeed + (md->scatSeed >= 0.0f ? 0.5f : -0.5f));
    std::vector<Sample> S; Samples(W, md->scatEmit, count, seed, S);
    // lo de cada copia, de OTRO flujo: siempre 5 numeros por copia (cambiar la escala no mueve nada de lugar)
    ScatRng r(seed ^ 0x5bd1e995u);
    rt.mats.resize(S.size() * 16);
    rt.pick.resize(S.size());
    for (size_t i = 0; i < S.size(); i++) {
        const unsigned pk = r.Next();
        const float fs = r.F(), fr = r.F(), fd = r.F(), ft = r.F();
        rt.pick[i] = (int)(pk >> 1);
        const float scale = md->scatScale * (1.0f - md->scatScaleRand * fs);
        const Vector3 up = md->scatAlign ? S[i].n : Vector3(0, 1, 0);
        CopyMatrix(S[i].p, up, md->scatRotRand * 2.0f * kPi * fr, 2.0f * kPi * fd, md->scatTiltRand * (kPi / 180.0f) * ft,
                   scale, md->scatOffset, &rt.mats[i * 16]);
    }
    // el emisor apagado: en modo unida se van sus caras (en instancias lo saltea el dibujo, W3dScatterRender)
    if (md->scatMode == 0 && !md->scatShowEmitter) {
        W.F.clear(); W.Fmat.clear(); W.Fsmooth.clear(); W.Fuv.clear(); W.Fcol.clear();
    }
    if (md->scatMode != 0 || pool.empty() || S.empty() || gScatterDepth > 1) return;
    // ---- MODO UNIDA: la geometria de cada copia se suma a W (cada malla de cada fuente, evaluada una sola vez) ----
    gScatterDepth++;
    const int baseParts = (int)emitter->materialsGroup.size();
    std::map<Mesh*, PolyMesh> polys;
    std::map<Material*, int> matIdx;
    for (size_t k = 0; k < W.extraMats.size(); k++) matIdx[W.extraMats[k]] = (int)k;
    std::vector< std::vector<std::pair<Mesh*, Matrix4> > > pieces(pool.size());
    Matrix4 I; I.Identity();
    for (size_t u = 0; u < pool.size(); u++) {
        MeshesOf(pool[u], I, pieces[u], 0);
        for (size_t k = 0; k < pieces[u].size(); k++) {
            Mesh* sm = pieces[u][k].first;
            if (polys.count(sm)) continue;
            PolyMesh& B = polys[sm];
            W3dMeshPolyEvaluated(sm, B);
        }
    }
    for (size_t i = 0; i < S.size(); i++) {
        const size_t u = (size_t)rt.pick[i] % pool.size();
        Matrix4 CS; W3dScatterSourceMatrix(&rt.mats[i * 16], pool[u], CS);
        for (size_t k = 0; k < pieces[u].size(); k++) {
            Mesh* sm = pieces[u][k].first;
            const PolyMesh& B = polys[sm];
            if (B.F.empty()) continue;
            const Matrix4 M = CS * pieces[u][k].second;
            const bool flip = Det3(M) < 0.0f;   // (una escala negativa da vuelta las caras)
            const int base = (int)W.P.size();
            for (size_t p = 0; p < B.P.size(); p++) W.P.push_back(M * B.P[p]);
            for (size_t f = 0; f < B.F.size(); f++) {
                std::vector<int> F = B.F[f];
                for (size_t c = 0; c < F.size(); c++) F[c] += base;
                std::vector<float> uv = f < B.Fuv.size() ? B.Fuv[f] : std::vector<float>(F.size() * 2, 0.0f);
                std::vector<unsigned char> col = f < B.Fcol.size() ? B.Fcol[f] : std::vector<unsigned char>(F.size() * 4, 255);
                if (flip) {
                    std::vector<int> rf(F.rbegin(), F.rend()); F.swap(rf);
                    std::vector<float> ru; for (int c = (int)uv.size() / 2 - 1; c >= 0; c--) { ru.push_back(uv[(size_t)c * 2]); ru.push_back(uv[(size_t)c * 2 + 1]); }
                    uv.swap(ru);
                    std::vector<unsigned char> rc; for (int c = (int)col.size() / 4 - 1; c >= 0; c--) for (int q = 0; q < 4; q++) rc.push_back(col[(size_t)c * 4 + (size_t)q]);
                    col.swap(rc);
                }
                // su material: el de su parte en la malla fuente, como parte EXTRA de la generada
                const int g = f < B.Fmat.size() ? B.Fmat[f] : 0;
                Material* mt = (g >= 0 && g < (int)sm->materialsGroup.size()) ? sm->materialsGroup[(size_t)g].material : NULL;
                std::map<Material*, int>::iterator it = matIdx.find(mt);
                int k2;
                if (it == matIdx.end()) { k2 = (int)W.extraMats.size(); W.extraMats.push_back(mt); matIdx[mt] = k2; }
                else k2 = it->second;
                int sm2 = f < B.Fsmooth.size() ? B.Fsmooth[f] : -1;
                if (sm2 < 0) sm2 = sm->meshSmooth ? 1 : 0;   // (hereda el de SU malla, no el del emisor)
                W.F.push_back(F); W.Fmat.push_back(baseParts + k2); W.Fsmooth.push_back(sm2);
                W.Fuv.push_back(uv); W.Fcol.push_back(col);
            }
        }
    }
    gScatterDepth--;
}

bool W3dScatterHasInstances(const Mesh* m) {
    if (!m) return false;
    for (size_t i = 0; i < m->modifiers.size(); i++) {
        const Modifier* md = m->modifiers[i];
        if (ScatterOn(m, md) && md->scatMode == 1 && !md->scatRt.mats.empty()) return true;
    }
    return false;
}

namespace {
// una fuente dibujada en el lugar de la copia: ella (sin su lugar) y sus hijos visibles, con sus matrices locales
void RenderUnit(Object* o, int depth) {
    if (!o || depth > 32) return;
    o->RenderObject();
    for (size_t i = 0; i < o->Childrens.size(); i++) {
        Object* c = o->Childrens[i];
        if (!c || !c->visible) continue;
        Matrix4 L; c->GetMatrix(L);
        w3dEngine::PushMatrix();
        w3dEngine::MultMatrix(L.m);
        RenderUnit(c, depth + 1);
        w3dEngine::PopMatrix();
    }
}

void PiecesOf(Object* o, const Matrix4& W, std::vector<W3dScatterPiece>& out, int depth) {
    if (!o || depth > 32) return;
    if (o->getType() == ObjectType::mesh) { W3dScatterPiece p; p.m = (Mesh*)o; p.W = W; out.push_back(p); }
    for (size_t i = 0; i < o->Childrens.size(); i++) {
        Object* c = o->Childrens[i];
        if (!c || !c->visible) continue;
        Matrix4 L; c->GetMatrix(L);
        PiecesOf(c, W * L, out, depth + 1);
    }
}
}   // namespace

bool W3dScatterRender(Mesh* m, bool draw) {
    if (!m || m->modifiers.empty()) return false;
    bool hide = false;
    for (size_t i = 0; i < m->modifiers.size(); i++) {
        Modifier* md = m->modifiers[i];
        if (!ScatterOn(m, md) || md->scatMode != 1) continue;
        if (!md->scatShowEmitter) hide = true;
        if (!draw || md->scatRt.mats.empty() || gScatterDepth > 1) continue;
        std::vector<Object*> pool; W3dScatterPool(md, m, pool);
        if (pool.empty()) continue;
        gScatterDepth++;
        // (como la Instance: nada diferido adentro -calcomanias, luces-: se dibujaria una vez, en el lugar del original)
        const bool difInline = w3dDecalsInline;
        w3dDecalsInline = true;
        const size_t n = md->scatRt.pick.size();
        for (size_t k = 0; k < n; k++) {
            Object* src = pool[(size_t)md->scatRt.pick[k] % pool.size()];
            Matrix4 M; W3dScatterSourceMatrix(&md->scatRt.mats[k * 16], src, M);
            const bool flip = Det3(M) < 0.0f;
            w3dEngine::PushMatrix();
            w3dEngine::MultMatrix(M.m);
            if (flip) w3dEngine::FrontFace(false);
            RenderUnit(src, 0);
            if (flip) w3dEngine::FrontFace(true);
            w3dEngine::PopMatrix();
        }
        w3dDecalsInline = difInline;
        gScatterDepth--;
    }
    // (en Edit Mode el emisor se ve siempre: es lo que se edita)
    return hide && (Object*)m != g_editMesh;
}

// el dibujo de las copias lo pide Mesh::RenderObject por un gancho (un juego 2D compila Mesh.cpp sin esta carpeta)
extern bool (*g_w3dScatterRenderHook)(Mesh* m, bool draw);
namespace { struct ScatterHook { ScatterHook() { g_w3dScatterRenderHook = W3dScatterRender; } } gScatterHook; }

void W3dScatterPieces(Mesh* emitter, const Matrix4& emitterWorld, std::vector<W3dScatterPiece>& out) {
    if (!emitter || gScatterDepth > 1) return;
    for (size_t i = 0; i < emitter->modifiers.size(); i++) {
        Modifier* md = emitter->modifiers[i];
        if (!ScatterOn(emitter, md) || md->scatMode != 1 || md->scatRt.mats.empty()) continue;
        std::vector<Object*> pool; W3dScatterPool(md, emitter, pool);
        if (pool.empty()) continue;
        for (size_t k = 0; k < md->scatRt.pick.size(); k++) {
            Object* src = pool[(size_t)md->scatRt.pick[k] % pool.size()];
            Matrix4 M; W3dScatterSourceMatrix(&md->scatRt.mats[k * 16], src, M);
            PiecesOf(src, emitterWorld * M, out, 0);
        }
    }
}
