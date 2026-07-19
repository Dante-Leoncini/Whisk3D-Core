// ============================================================================
//  w3dGltf.h — loader + renderer glTF/GLB del CORE (reusable por apps y juegos,
//  no solo el editor). Solo glTF/GLB (el editor sigue teniendo OBJ/FBX de extras).
//  Usa el motor del Core (w3dEngine gfx + math) + stb para la textura. Carga
//  desde un W3dPack: acepta GLB binario (chunk JSON + BIN) y glTF texto (JSON con
//  buffer data-uri o buffer/imagen externos que salen del MISMO pack). Renderiza
//  ANIMADO (rota los nodos con animacion) en vista ORTOGRAFICA (o la que arme el
//  caller). Draw NO-indexado (expande por indices) -> sin limite 16-bit. Cubre lo
//  que exporta Whisk3D: POSITION/NORMAL/TEXCOORD_0 + baseColor/textura + animacion
//  de rotacion de objeto. Header-only (funciones inline) -> el que lo incluye no
//  compila un .cpp extra. C++03 (Symbian-safe).
// ============================================================================
#ifndef W3D_GLTF_H
#define W3D_GLTF_H

#include "w3dGraphics.h"
#include "math/Matrix4.h"
#include "math/Vector3.h"
#include "math/Quaternion.h"
#include "w3dTexture.h"     // UploadRGBA / DeleteTexture
#include "stb/stb_image.h"  // stbi_load_from_memory
#include "io/W3dPack.h"
#include "w3dlog.h"
#include <vector>
#include <string>
#include <map>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

namespace gltf {

namespace gfx = w3dEngine;

// ---------- JSON minimo (objeto/array/string/numero/bool/null) ----------
struct JVal {
    enum Type { OBJ, ARR, STR, NUM, BOOL, NUL } t;
    double num; bool b; std::string str;
    std::vector<JVal> arr;                 // ARR: elementos ; OBJ: valores (paralelo a keys)
    std::vector<std::string> keys;         // OBJ: claves
    JVal() : t(NUL), num(0), b(false) {}
    const JVal* find(const char* k) const {
        if (t != OBJ) return 0;
        for (size_t i = 0; i < keys.size(); i++) if (keys[i] == k) return &arr[i];
        return 0;
    }
    bool has(const char* k) const { return find(k) != 0; }
    int    getI(const char* k, int d) const { const JVal* v = find(k); return (v && v->t == NUM) ? (int)v->num : d; }
    double getN(const char* k, double d) const { const JVal* v = find(k); return (v && v->t == NUM) ? v->num : d; }
    std::string getS(const char* k, const char* d) const { const JVal* v = find(k); return (v && v->t == STR) ? v->str : std::string(d); }
    bool   getB(const char* k, bool d) const { const JVal* v = find(k); return (v && v->t == BOOL) ? v->b : d; }
    size_t size() const { return arr.size(); }
};

struct JParser {
    const char* p; const char* end; bool ok;
    JParser(const char* s, size_t n) : p(s), end(s + n), ok(true) {}
    void ws() { while (p < end && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) p++; }
    bool str(std::string& out) {
        if (p >= end || *p != '"') { ok = false; return false; } p++;
        while (p < end) { char c = *p++;
            if (c == '"') return true;
            if (c == '\\') { if (p >= end) break; char e = *p++;
                switch (e) { case 'n': out+='\n'; break; case 't': out+='\t'; break; case 'r': out+='\r'; break;
                    case '/': out+='/'; break; case '\\': out+='\\'; break; case '"': out+='"'; break; case 'b': out+='\b'; break; case 'f': out+='\f'; break;
                    case 'u': { if (end - p >= 4) { p += 4; out += '?'; } break; } // \uXXXX: no lo necesitamos (nombres ascii)
                    default: out += e; } }
            else out += c; }
        ok = false; return false;
    }
    bool value(JVal& v) {
        ws(); if (p >= end) { ok = false; return false; }
        char c = *p;
        if (c == '{') return object(v);
        if (c == '[') return array(v);
        if (c == '"') { v.t = JVal::STR; return str(v.str); }
        if (c == 't') { if (end-p>=4 && !strncmp(p,"true",4)) { p+=4; v.t=JVal::BOOL; v.b=true; return true; } ok=false; return false; }
        if (c == 'f') { if (end-p>=5 && !strncmp(p,"false",5)) { p+=5; v.t=JVal::BOOL; v.b=false; return true; } ok=false; return false; }
        if (c == 'n') { if (end-p>=4 && !strncmp(p,"null",4)) { p+=4; v.t=JVal::NUL; return true; } ok=false; return false; }
        // numero
        const char* s = p; while (p < end && (*p=='-'||*p=='+'||*p=='.'||*p=='e'||*p=='E'||(*p>='0'&&*p<='9'))) p++;
        if (p == s) { ok = false; return false; }
        v.t = JVal::NUM; v.num = atof(std::string(s, p - s).c_str()); return true;
    }
    bool object(JVal& v) {
        v.t = JVal::OBJ; p++; ws();
        if (p < end && *p == '}') { p++; return true; }
        while (p < end) { ws(); std::string key; if (!str(key)) return false;
            ws(); if (p >= end || *p != ':') { ok=false; return false; } p++;
            JVal val; if (!value(val)) return false;
            v.keys.push_back(key); v.arr.push_back(val);
            ws(); if (p >= end) { ok=false; return false; }
            if (*p == ',') { p++; continue; } if (*p == '}') { p++; return true; }
            ok = false; return false; }
        ok = false; return false;
    }
    bool array(JVal& v) {
        v.t = JVal::ARR; p++; ws();
        if (p < end && *p == ']') { p++; return true; }
        while (p < end) { JVal el; if (!value(el)) return false; v.arr.push_back(el);
            ws(); if (p >= end) { ok=false; return false; }
            if (*p == ',') { p++; continue; } if (*p == ']') { p++; return true; }
            ok = false; return false; }
        ok = false; return false;
    }
};

// ---------- base64 -> bytes (para los buffers data-uri) ----------
inline int b64v(char c){ if(c>='A'&&c<='Z')return c-'A'; if(c>='a'&&c<='z')return c-'a'+26; if(c>='0'&&c<='9')return c-'0'+52; if(c=='+')return 62; if(c=='/')return 63; return -1; }
inline void b64decode(const char* s, size_t n, std::vector<unsigned char>& out){
    int acc=0, bits=0;
    for (size_t i=0;i<n;i++){ int v=b64v(s[i]); if(v<0)continue; acc=(acc<<6)|v; bits+=6; if(bits>=8){ bits-=8; out.push_back((unsigned char)((acc>>bits)&0xFF)); } }
}

// ---------- datos renderizables ----------
struct Prim { std::vector<float> pos, uv, nrm; int nverts; int material; bool hasUV; };
struct NodeT { int mesh; Vector3 T; Quaternion R; Vector3 S; };
struct Mat { unsigned tex; float base[4]; bool doubleSided; bool chrome; int reflectMode; }; // chrome = reflejo por SW
struct RotKey { float t; Quaternion q; };

struct Model {
    std::vector<std::vector<Prim> > meshes; // meshes[m] = sus primitivas
    std::vector<NodeT> nodes;
    std::vector<Mat> mats;
    std::map<int, std::vector<RotKey> > nodeRot; // nodo -> keyframes de rotacion (segundos)
    float duration; // segundos
    Vector3 bmin, bmax;
    float fitAspect; // ancho/alto que ocupa el logo al encuadrarlo (con margen de giro) -> lo usa el caller p/ el viewport
    Model() : duration(0), fitAspect(1.0f) { bmin = Vector3(1e9f,1e9f,1e9f); bmax = Vector3(-1e9f,-1e9f,-1e9f); }
};

inline Matrix4 NodeMatrix(const NodeT& nd); // (def mas abajo) matriz local T*R*S de un nodo

// lee un accessor de floats (SCALAR/VEC2/VEC3/VEC4) desde un buffer ya decodificado
inline bool readAcc(const JVal& doc, const std::vector<std::vector<unsigned char> >& bufs,
                    int accIdx, std::vector<float>& out, int& comps) {
    const JVal* accs = doc.find("accessors"); if (!accs || accIdx < 0 || accIdx >= (int)accs->size()) return false;
    const JVal& a = accs->arr[accIdx];
    int bv = a.getI("bufferView", -1); if (bv < 0) return false;
    int count = a.getI("count", 0);
    std::string type = a.getS("type", "SCALAR");
    comps = (type=="SCALAR")?1:(type=="VEC2")?2:(type=="VEC3")?3:(type=="VEC4")?4:(type=="MAT4")?16:0;
    if (comps == 0) return false;
    int ct = a.getI("componentType", 5126); // 5126 float, 5123 ushort, 5125 uint, 5121 ubyte
    const JVal* bvs = doc.find("bufferViews"); if (!bvs || bv >= (int)bvs->size()) return false;
    const JVal& view = bvs->arr[bv];
    int buf = view.getI("buffer", 0); if (buf < 0 || buf >= (int)bufs.size()) return false;
    size_t off = (size_t)view.getI("byteOffset", 0) + (size_t)a.getI("byteOffset", 0);
    const unsigned char* base = bufs[buf].empty() ? 0 : &bufs[buf][0];
    size_t blen = bufs[buf].size();
    out.resize((size_t)count * comps);
    for (int i = 0; i < count * comps; i++) {
        size_t o; float val = 0;
        if (ct == 5126) { o = off + (size_t)i*4; if (o+4<=blen){ float f; memcpy(&f, base+o, 4); val=f; } }
        else if (ct == 5123) { o = off + (size_t)i*2; if (o+2<=blen){ unsigned short s; memcpy(&s, base+o, 2); val=(float)s; } }
        else if (ct == 5125) { o = off + (size_t)i*4; if (o+4<=blen){ unsigned int u; memcpy(&u, base+o, 4); val=(float)u; } }
        else if (ct == 5121) { o = off + (size_t)i; if (o+1<=blen) val=(float)base[o]; }
        else return false;
        out[i] = val;
    }
    return true;
}
// lee un accessor de indices (a enteros)
inline bool readIdx(const JVal& doc, const std::vector<std::vector<unsigned char> >& bufs, int accIdx, std::vector<unsigned>& out) {
    std::vector<float> f; int c=0; if (!readAcc(doc, bufs, accIdx, f, c) || c != 1) return false;
    out.resize(f.size()); for (size_t i=0;i<f.size();i++) out[i]=(unsigned)(f[i]+0.5f); return true;
}

// carga el glTF (nombre en el pack) + sus imagenes (del mismo pack). true si ok.
// getter de un asset por nombre: lo resuelve un W3dPack, un directorio, etc. Devuelve puntero + largo (0 si falta).
typedef const unsigned char* (*GetAsset)(void* ctx, const char* name, size_t* len);

inline bool LoadG(GetAsset get, void* ctx, const char* gltfName, Model& M) {
    size_t glen0 = 0; const unsigned char* g0 = get(ctx, gltfName, &glen0);
    if (!g0 || !glen0) { w3dLogE("gltf: no encontre el glTF/GLB"); return false; }
    std::vector<unsigned char> gltfData(g0, g0 + glen0); // COPIA: estable durante TODO el parse (el getter reusa buffer)
    const unsigned char* gbytes = &gltfData[0]; size_t glen = glen0;

    // GLB binario (magic "glTF") vs glTF texto: en GLB el JSON y el buffer van en CHUNKS (len(4) type(4) data).
    const char* jsonPtr = (const char*)gbytes; size_t jsonLen = glen;
    const unsigned char* glbBin = 0; size_t glbBinLen = 0;
    if (glen >= 12 && gbytes[0]=='g' && gbytes[1]=='l' && gbytes[2]=='T' && gbytes[3]=='F') {
        size_t o = 12;
        while (o + 8 <= glen) {
            unsigned int clen = 0, ctype = 0; memcpy(&clen, gbytes+o, 4); memcpy(&ctype, gbytes+o+4, 4); o += 8;
            if ((size_t)clen > glen - o) break;
            if (ctype == 0x4E4F534Au)      { jsonPtr = (const char*)(gbytes+o); jsonLen = clen; } // 'JSON'
            else if (ctype == 0x004E4942u) { glbBin = gbytes+o; glbBinLen = clen; }               // 'BIN\0'
            o += clen; // los chunks GLB estan padeados a multiplo de 4 -> o sigue alineado
        }
    }
    JParser jp(jsonPtr, jsonLen); JVal doc;
    if (!jp.value(doc) || !jp.ok || doc.t != JVal::OBJ) { w3dLogE("gltf: JSON invalido"); return false; }

    // buffers: data-uri base64 (texto), el chunk BIN (GLB, buffer sin uri), o un archivo externo del MISMO pack.
    std::vector<std::vector<unsigned char> > bufs;
    const JVal* jbufs = doc.find("buffers");
    if (jbufs) for (size_t i=0;i<jbufs->size();i++) {
        std::string uri = jbufs->arr[i].getS("uri", "");
        std::vector<unsigned char> data;
        if (uri.empty()) { if (glbBin) data.assign(glbBin, glbBin + glbBinLen); } // GLB: buffer 0 = chunk BIN
        else { size_t comma = uri.find("base64,");
            if (comma != std::string::npos) b64decode(uri.c_str()+comma+7, uri.size()-comma-7, data);   // data-uri embebido
            else if (uri.compare(0,5,"data:") != 0) {                                                    // archivo externo (pack o disco)
                size_t bl=0; const unsigned char* bb = get(ctx, uri.c_str(), &bl); if (bb && bl) data.assign(bb, bb+bl); } }
        bufs.push_back(data);
    }

    // texturas -> images[source].uri -> Get del pack -> stb -> GL. Cachea por indice de textura.
    std::vector<unsigned> texGL;
    const JVal* jtex = doc.find("textures"); const JVal* jimg = doc.find("images");
    if (jtex && jimg) for (size_t i=0;i<jtex->size();i++) {
        unsigned id = 0; int src = jtex->arr[i].getI("source", -1);
        if (src >= 0 && src < (int)jimg->size()) {
            std::string uri = jimg->arr[src].getS("uri", "");
            if (!uri.empty() && uri.compare(0,5,"data:")!=0) {
                size_t ilen=0; const unsigned char* ib = get(ctx, uri.c_str(), &ilen);
                if (ib && ilen) { int w=0,h=0,ch=0; unsigned char* rgba = stbi_load_from_memory(ib,(int)ilen,&w,&h,&ch,4);
                    if (rgba) { id = gfx::UploadRGBA(rgba, w, h, true); stbi_image_free(rgba); } }
            }
        }
        texGL.push_back(id);
    }

    // materiales
    const JVal* jmats = doc.find("materials");
    if (jmats) for (size_t i=0;i<jmats->size();i++) { const JVal& jm = jmats->arr[i];
        Mat mt; mt.tex=0; mt.base[0]=mt.base[1]=mt.base[2]=mt.base[3]=1.0f; mt.doubleSided = jm.getB("doubleSided", false);
        mt.chrome = false; mt.reflectMode = 0;
        const JVal* pbr = jm.find("pbrMetallicRoughness");
        if (pbr) { const JVal* bc = pbr->find("baseColorFactor");
            if (bc && bc->size()==4) for (int k=0;k<4;k++) mt.base[k]=(float)bc->arr[k].num;
            const JVal* bct = pbr->find("baseColorTexture");
            if (bct) { int ti = bct->getI("index", -1); if (ti>=0 && ti<(int)texGL.size()) mt.tex = texGL[ti]; } }
        const JVal* ex = jm.find("extras"); // reflejo (chrome) por SOFTWARE + su modo (Whisk3D)
        if (ex) { mt.chrome = ex->getB("w3d_chrome", false); mt.reflectMode = ex->getI("w3d_reflectMode", 0); }
        M.mats.push_back(mt);
    }

    // meshes -> primitivas expandidas (no indexadas)
    const JVal* jmeshes = doc.find("meshes");
    if (jmeshes) for (size_t mi=0;mi<jmeshes->size();mi++) {
        std::vector<Prim> prims; const JVal* jp2 = jmeshes->arr[mi].find("primitives");
        if (jp2) for (size_t pi=0;pi<jp2->size();pi++) { const JVal& pr = jp2->arr[pi];
            const JVal* at = pr.find("attributes"); if (!at) continue;
            std::vector<float> POS, NRM, UV; int pc=0,nc=0,uc=0;
            if (!readAcc(doc, bufs, at->getI("POSITION", -1), POS, pc) || pc != 3) continue;
            bool hasN = readAcc(doc, bufs, at->getI("NORMAL", -1), NRM, nc) && nc==3;
            bool hasU = readAcc(doc, bufs, at->getI("TEXCOORD_0", -1), UV, uc) && uc==2;
            std::vector<unsigned> idx; int a = pr.getI("indices", -1);
            if (a >= 0) readIdx(doc, bufs, a, idx);
            else { size_t nv = POS.size()/3; idx.resize(nv); for (size_t k=0;k<nv;k++) idx[k]=(unsigned)k; }
            Prim P; P.material = pr.getI("material", -1); P.hasUV = hasU; P.nverts = (int)idx.size();
            for (size_t k=0;k<idx.size();k++) { unsigned v = idx[k];
                P.pos.push_back(POS[v*3]); P.pos.push_back(POS[v*3+1]); P.pos.push_back(POS[v*3+2]);
                if (hasN) { P.nrm.push_back(NRM[v*3]); P.nrm.push_back(NRM[v*3+1]); P.nrm.push_back(NRM[v*3+2]); }
                if (hasU) { P.uv.push_back(UV[v*2]); P.uv.push_back(UV[v*2+1]); }
            }
            prims.push_back(P);
        }
        M.meshes.push_back(prims);
    }

    // nodos (TRS)
    const JVal* jnodes = doc.find("nodes");
    if (jnodes) for (size_t i=0;i<jnodes->size();i++) { const JVal& n = jnodes->arr[i];
        NodeT nd; nd.mesh = n.getI("mesh", -1); nd.T=Vector3(0,0,0); nd.S=Vector3(1,1,1); nd.R=Quaternion(1,0,0,0);
        const JVal* T=n.find("translation"); if (T&&T->size()==3) nd.T=Vector3((float)T->arr[0].num,(float)T->arr[1].num,(float)T->arr[2].num);
        const JVal* S=n.find("scale");       if (S&&S->size()==3) nd.S=Vector3((float)S->arr[0].num,(float)S->arr[1].num,(float)S->arr[2].num);
        const JVal* R=n.find("rotation");     if (R&&R->size()==4) nd.R=Quaternion((float)R->arr[3].num,(float)R->arr[0].num,(float)R->arr[1].num,(float)R->arr[2].num); // (w,x,y,z)
        M.nodes.push_back(nd);
    }

    // animaciones: canales de ROTACION que apuntan a un nodo -> keyframes (slerp en playback)
    const JVal* janims = doc.find("animations");
    if (janims) for (size_t ai=0;ai<janims->size();ai++) { const JVal& an = janims->arr[ai];
        const JVal* chans = an.find("channels"); const JVal* samps = an.find("samplers"); if (!chans||!samps) continue;
        for (size_t ci=0;ci<chans->size();ci++) { const JVal& ch = chans->arr[ci];
            const JVal* tgt = ch.find("target"); if (!tgt) continue;
            if (tgt->getS("path","") != "rotation") continue; // por ahora solo rotacion (es lo que anima el logo)
            int node = tgt->getI("node", -1); if (node < 0) continue;
            int si = ch.getI("sampler", -1); if (si<0 || si>=(int)samps->size()) continue;
            const JVal& sm = samps->arr[si];
            std::vector<float> times, vals; int tc=0, vc=0;
            if (!readAcc(doc, bufs, sm.getI("input",-1), times, tc) || tc!=1) continue;
            if (!readAcc(doc, bufs, sm.getI("output",-1), vals, vc) || vc!=4) continue;
            std::vector<RotKey>& keys = M.nodeRot[node];
            for (size_t k=0;k<times.size();k++) { RotKey rk; rk.t=times[k];
                rk.q=Quaternion(vals[k*4+3], vals[k*4], vals[k*4+1], vals[k*4+2]); keys.push_back(rk);
                if (times[k] > M.duration) M.duration = times[k]; }
        }
    }

    // bounding box en MUNDO (nodos base) -> para encuadrar la orto
    for (size_t i=0;i<M.nodes.size();i++) { NodeT& nd = M.nodes[i]; if (nd.mesh<0 || nd.mesh>=(int)M.meshes.size()) continue;
        Matrix4 W = NodeMatrix(nd);
        std::vector<Prim>& prims = M.meshes[nd.mesh];
        for (size_t pi=0;pi<prims.size();pi++){ Prim& P=prims[pi];
            for (int v=0; v<P.nverts; v++){ Vector3 p(P.pos[v*3],P.pos[v*3+1],P.pos[v*3+2]); Vector3 w = W * p;
                if(w.x<M.bmin.x)M.bmin.x=w.x; if(w.y<M.bmin.y)M.bmin.y=w.y; if(w.z<M.bmin.z)M.bmin.z=w.z;
                if(w.x>M.bmax.x)M.bmax.x=w.x; if(w.y>M.bmax.y)M.bmax.y=w.y; if(w.z>M.bmax.z)M.bmax.z=w.z; } }
    }
    // aspect del encuadre: ancho = max(extX, extZ) (el giro en Y puede llevar el ancho al radio en Z), alto = extY.
    { Vector3 e = M.bmax - M.bmin; if (e.x < 0) e = Vector3(1,1,1);
      float wdt = e.x > e.z ? e.x : e.z; float hgt = e.y;
      M.fitAspect = (hgt > 1e-5f) ? (wdt / hgt) : 1.0f; if (M.fitAspect < 0.05f) M.fitAspect = 0.05f; if (M.fitAspect > 20.0f) M.fitAspect = 20.0f; }
    w3dLogf("gltf: %d meshes, %d nodes, %d mats, dur=%.2fs, aspect=%.2f", (int)M.meshes.size(), (int)M.nodes.size(), (int)M.mats.size(), M.duration, M.fitAspect);
    return true;
}

// ---------- entradas: desde un W3dPack (cifrado) o desde ARCHIVOS (disco / VFS de emscripten) ----------
inline const unsigned char* PackGet(void* c, const char* name, size_t* len) { return ((w3dEngine::W3dPack*)c)->Get(name, len); }
// carga un glTF/GLB (y sus texturas/buffers externos) desde un W3dPack CIFRADO.
inline bool Load(w3dEngine::W3dPack& pack, const char* gltfName, Model& M) { return LoadG(PackGet, &pack, gltfName, M); }

struct FileCtx { std::string dir; std::vector<unsigned char> buf; }; // dir base + buffer reusado por Get
inline const unsigned char* FileGet(void* c, const char* name, size_t* len) {
    FileCtx* fc = (FileCtx*)c; std::string path = fc->dir;
    if (!path.empty() && path[path.size()-1] != '/') path += "/"; path += name;
    fc->buf.clear(); FILE* f = fopen(path.c_str(), "rb"); if (!f) { *len = 0; return 0; }
    fseek(f,0,SEEK_END); long sz = ftell(f); fseek(f,0,SEEK_SET);
    if (sz > 0) { fc->buf.resize((size_t)sz); if (fread(&fc->buf[0],1,(size_t)sz,f) != (size_t)sz) fc->buf.clear(); }
    fclose(f); *len = fc->buf.size(); return fc->buf.empty() ? 0 : &fc->buf[0];
}
// carga un glTF/GLB desde un ARCHIVO. 'dir' = carpeta base (de ahi salen las texturas/buffers externos).
// Ej (web con --embed-file assets@/assets):  gltf::LoadFile("assets", "whisk3d_logo.glb", model);
inline bool LoadFile(const char* dir, const char* gltfName, Model& M) { FileCtx fc; fc.dir = dir ? dir : "."; return LoadG(FileGet, &fc, gltfName, M); }

// matriz local de un nodo (T * R * S)
inline Matrix4 NodeMatrix(const NodeT& nd) {
    Matrix4 T; T.Identity(); T.m[12]=nd.T.x; T.m[13]=nd.T.y; T.m[14]=nd.T.z;
    Matrix4 R = nd.R.ToMatrix();
    Matrix4 S; S.Identity(); S.m[0]=nd.S.x; S.m[5]=nd.S.y; S.m[10]=nd.S.z;
    return T * R * S;
}

// rotacion animada del nodo en 'timeSec' (loop). Devuelve la base si no tiene animacion.
inline Quaternion NodeRotAt(const Model& M, int node, const Quaternion& base, float timeSec) {
    std::map<int, std::vector<RotKey> >::const_iterator it = M.nodeRot.find(node);
    if (it == M.nodeRot.end() || it->second.empty()) return base;
    const std::vector<RotKey>& k = it->second;
    if (k.size()==1) return k[0].q;
    float t = timeSec; float dur = k.back().t; if (dur > 0){ t = fmodf(t, dur); if (t<0) t+=dur; }
    for (size_t i=1;i<k.size();i++) if (t <= k[i].t) {
        float span = k[i].t - k[i-1].t; float f = span>1e-6f ? (t - k[i-1].t)/span : 0.0f;
        return Quaternion::Slerp(k[i-1].q, k[i].q, f);
    }
    return k.back().q;
}

// UV del REFLEJO por SOFTWARE de un vertice (misma matematica que ChromeUVCorner del Core): sphere-map en espacio
// de OJO (modo Matcap/Sphere) o equirect 360. wp/wn = posicion/normal en MUNDO; cr/cu/cf/cam = base de camara.
inline void ChromeUV(const Vector3& wp, const Vector3& wn, const Vector3& cam,
                     const Vector3& cr, const Vector3& cu, const Vector3& cf, bool equirect, float& u, float& v) {
    const float PI = 3.14159265358979f;
    if (equirect) {
        Vector3 I = (wp - cam).Normalized();
        float dd = I.x*wn.x + I.y*wn.y + I.z*wn.z;
        Vector3 R(I.x-2*dd*wn.x, I.y-2*dd*wn.y, I.z-2*dd*wn.z);
        float ry = R.y<-1.0f?-1.0f:(R.y>1.0f?1.0f:R.y);
        u = atan2f(R.z, R.x)/(2.0f*PI)+0.5f; v = acosf(ry)/PI;
    } else {
        Vector3 rel = wp - cam;
        Vector3 ep(rel.x*cr.x+rel.y*cr.y+rel.z*cr.z, rel.x*cu.x+rel.y*cu.y+rel.z*cu.z, -(rel.x*cf.x+rel.y*cf.y+rel.z*cf.z));
        Vector3 en(wn.x*cr.x+wn.y*cr.y+wn.z*cr.z, wn.x*cu.x+wn.y*cu.y+wn.z*cu.z, -(wn.x*cf.x+wn.y*cf.y+wn.z*cf.z));
        Vector3 I = ep.Normalized();
        float dd = I.x*en.x+I.y*en.y+I.z*en.z;
        Vector3 R(I.x-2*dd*en.x, I.y-2*dd*en.y, I.z-2*dd*en.z);
        float mm = 2.0f*sqrtf(R.x*R.x+R.y*R.y+(R.z+1.0f)*(R.z+1.0f)); if (mm<1e-5f) mm=1e-5f;
        u = R.x/mm+0.5f; v = 1.0f - (R.y/mm+0.5f); // flip-V (matchea GL_SPHERE_MAP)
    }
}
// aplica solo la parte 3x3 (rotacion+escala) de W a una direccion (normal). col-major.
inline Vector3 Rot3(const Matrix4& W, const Vector3& n) {
    return Vector3(W.m[0]*n.x + W.m[4]*n.y + W.m[8]*n.z,
                   W.m[1]*n.x + W.m[5]*n.y + W.m[9]*n.z,
                   W.m[2]*n.x + W.m[6]*n.y + W.m[10]*n.z);
}

// dibuja el modelo animado en 'timeSec', en el viewport (vx,vy,vw,vh), vista ORTOGRAFICA que encuadra el logo
// mirando de frente (-Z).
inline void Draw(Model& M, float timeSec, int vx, int vy, int vw, int vh) {
    if (vw<1) vw=1; if (vh<1) vh=1;
    // encuadre: fit del bounding XY del logo al viewport (letterbox), con margen para la rotacion (el logo gira
    // en Y -> su ancho proyectado puede llegar al radio en Z; se toma max(ancho/2, radioZ) como semi-ancho).
    Vector3 c = (M.bmin + M.bmax) * 0.5f;
    Vector3 ext = M.bmax - M.bmin;
    float r = ext.x; if (ext.y>r) r=ext.y; if (ext.z>r) r=ext.z; if (r<1e-4f) r=1.0f;
    float exM = ext.x*0.5f; float rz = ext.z*0.5f; if (rz>exM) exM=rz; // semi-ancho considerando el giro
    float ey  = ext.y*0.5f;
    if (exM<1e-4f) exM=1.0f; if (ey<1e-4f) ey=1.0f;
    exM *= 1.12f; ey *= 1.12f; // margen
    float aspect = (float)vw/(float)vh;
    float hx = exM, hy = ey;
    if (exM/ey > aspect) hy = exM/aspect; else hx = ey*aspect; // letterbox: el logo entra entero, centrado
    gfx::Viewport(vx,vy,vw,vh);
    gfx::Enable(gfx::DepthTest);
    gfx::Disable(gfx::Lighting);
    gfx::Disable(gfx::Blend);

    gfx::MatrixMode(gfx::Projection); gfx::LoadIdentity();
    gfx::Ortho(-hx, hx, -hy, hy, -r*4.0f, r*4.0f);
    gfx::MatrixMode(gfx::ModelView);  gfx::LoadIdentity();
    // "camara": centrar el logo (miramos de frente, -Z). Base de camara en MUNDO para el reflejo por SW (chrome).
    Matrix4 view; view.Identity(); view.m[12]=-c.x; view.m[13]=-c.y; view.m[14]=-c.z;
    gfx::LoadMatrix(view.m);
    Vector3 cr(1,0,0), cu(0,1,0), cf(0,0,-1);   // ejes de la camara (orto de frente)
    Vector3 cam = c + Vector3(0,0, r*3.0f);      // ojo en +Z
    static std::vector<float> ruv;               // UV del reflejo (reusado por primitiva)

    for (size_t i=0;i<M.nodes.size();i++) { NodeT nd = M.nodes[i]; if (nd.mesh<0 || nd.mesh>=(int)M.meshes.size()) continue;
        nd.R = NodeRotAt(M, (int)i, nd.R, timeSec);   // rotacion animada
        Matrix4 W = NodeMatrix(nd);
        gfx::PushMatrix(); gfx::MultMatrix(W.m);
        std::vector<Prim>& prims = M.meshes[nd.mesh];
        for (size_t pi=0;pi<prims.size();pi++){ Prim& P = prims[pi]; if (P.nverts<3) continue;
            Mat mt; mt.tex=0; mt.base[0]=mt.base[1]=mt.base[2]=mt.base[3]=1.0f; mt.doubleSided=false; mt.chrome=false; mt.reflectMode=0;
            if (P.material>=0 && P.material<(int)M.mats.size()) mt = M.mats[P.material];
            if (mt.doubleSided) gfx::Disable(gfx::CullFace); else gfx::Enable(gfx::CullFace);
            gfx::Color4f(mt.base[0], mt.base[1], mt.base[2], mt.base[3]);
            gfx::EnableArray(gfx::VertexArray); gfx::VertexPointer3f(0, &P.pos[0]);
            bool usoChrome = (mt.chrome && mt.tex && !P.nrm.empty());
            if (usoChrome) {
                // REFLEJO por SOFTWARE: la UV se calcula de la normal en MUNDO -> el metal "refleja" y cambia al
                // ROTAR (no es una UV fija). Se recalcula por frame (la animacion mueve la normal).
                ruv.resize((size_t)P.nverts*2); bool eq = (mt.reflectMode == 2);
                for (int vv=0; vv<P.nverts; vv++) {
                    Vector3 lp(P.pos[vv*3],P.pos[vv*3+1],P.pos[vv*3+2]);
                    Vector3 ln(P.nrm[vv*3],P.nrm[vv*3+1],P.nrm[vv*3+2]);
                    Vector3 wp = W * lp; Vector3 wn = Rot3(W, ln).Normalized();
                    ChromeUV(wp, wn, cam, cr, cu, cf, eq, ruv[vv*2], ruv[vv*2+1]);
                }
                gfx::Enable(gfx::Texture2D); gfx::BindTexture(mt.tex); gfx::TexFilter(true); gfx::TexWrap(eq);
                gfx::EnableArray(gfx::TexCoordArray); gfx::TexCoordPointer2f(0, &ruv[0]);
            } else if (P.hasUV && mt.tex) {
                gfx::Enable(gfx::Texture2D); gfx::BindTexture(mt.tex); gfx::TexFilter(true); gfx::TexWrap(true);
                gfx::EnableArray(gfx::TexCoordArray); gfx::TexCoordPointer2f(0, &P.uv[0]);
            } else { gfx::Disable(gfx::Texture2D); gfx::DisableArray(gfx::TexCoordArray); }
            gfx::DrawTrianglesArray(P.nverts);
        }
        gfx::PopMatrix();
    }
    gfx::DisableArray(gfx::TexCoordArray);
}

} // namespace gltf
#endif
