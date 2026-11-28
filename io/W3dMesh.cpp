#include "W3dMesh.h"
#include "W3dText.h"
#include "W3dMeshBin.h"   // W3dMeshBinBlockKnown: el freno "requiere" del binario
#include "objects/Mesh.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <cstring>

// libera las capas de la malla SIN pasar por Mesh::FreeLayers: ese metodo vive
// en el EDITOR (libs/Whisk3DCore/edit/MeshEdit.cpp) y el juego compilado lo tiene STUBEADO en
// vacio, asi que llamarlo desde aca leakearia en el runtime. Este archivo entra
// en las dos builds y tiene que valerse por si mismo.
static void W3dmFreeLayers(Mesh* m) {
    for (size_t i = 0; i < m->uvMaps.size(); i++)       delete m->uvMaps[i];
    for (size_t i = 0; i < m->colorLayers.size(); i++)  delete m->colorLayers[i];
    for (size_t i = 0; i < m->vertexGroups.size(); i++) delete m->vertexGroups[i];
    for (size_t i = 0; i < m->uvGroups.size(); i++)     delete m->uvGroups[i];
    m->uvMaps.clear();       m->uvActiveMap = -1;
    m->colorLayers.clear();  m->activeColor = -1;
    m->vertexGroups.clear(); m->activeGroup = -1;
    m->uvGroups.clear();     m->uvActiveGroup = -1;
}

// ===========================================================================
//  VOCABULARIO CERRADO DE DOMINIOS
//  Cerrarlo en cinco no es una apuesta: en dos anios de Mesh.h todo lo que se
//  agrego cayo en uno de estos cinco lugares. Y cerrarlo es lo que garantiza
//  que dentro de cinco anios un lector VIEJO sepa QUE FORMA tiene un bloque que
//  no conoce, que es de lo que depende la preservacion.
// ===========================================================================
enum { W3DM_POINT = 0, W3DM_CORNER = 1, W3DM_FACE = 2, W3DM_EDGE = 3, W3DM_GLOBAL = 4, W3DM_DOMMAL = -1 };

static const char* W3dmDomName(int d) {
    switch (d) {
        case W3DM_POINT:   return "point";
        case W3DM_CORNER: return "corner";
        case W3DM_FACE:    return "face";
        case W3DM_EDGE:  return "edge";
        default:           return "global";
    }
}
static int W3dmDomOf(const std::string& s) {
    if (s == "point")   return W3DM_POINT;
    if (s == "corner") return W3DM_CORNER;
    if (s == "face")    return W3DM_FACE;
    if (s == "edge")  return W3DM_EDGE;
    if (s == "global")  return W3DM_GLOBAL;
    return W3DM_DOMMAL;
}

// bloques que ESTA version entiende (registro: Whisk3D/formato/bloques.tsv)
static const char* kBlocks[] = {
    "V", "F", "FPART", "FSHADE", "NRM", "UV", "COL", "UVREST",
    "SHARP", "SEAM", "EDGE", "POINT", "PARTS", "PARTMAT", "VG", "UVG", "RVMAP", "SHAPE", 0
};
bool W3dMeshBlockKnown(const std::string& name) {
    for (int i = 0; kBlocks[i]; i++) if (name == kBlocks[i]) return true;
    return false;
}

// nombres de primitiva (orden de MeshType). El numero tambien se acepta al leer.
static const char* kPrim[] = { "cube", "sphere", "ico", "plane", "vertex", "circle", "cone", "cylinder" };
static const int   kPrimN = 8;

// ---------------------------------------------------------------------------
//  clave de POSICION: los 12 bytes CRUDOS de los 3 floats. Es la MISMA que usa
//  Mesh::SharpEdgeKey (libs/Whisk3DCore/edit/MeshEdit.cpp) y por eso el escritor de floats
//  tiene que round-trippear EXACTO: si un decimal vuelve como otro float, la
//  costura no matchea y se pierde EN SILENCIO.
// ---------------------------------------------------------------------------
static std::string W3dmPosKey(const float* p) {
    char b[12]; memcpy(b, p, 12); return std::string(b, 12);
}
// LA POSICION CUANTIZADA (a 1e-4, la misma regla que el agrupamiento del editor, Mesh::ComputeBorders): dos render-verts
// "en el mismo lugar" con floats apenas distintos -la costura de una esfera UV (sin(2 pi) no es 0), un polo con -0.0-
// son el mismo punto. Con los bits exactos quedaban como puntos distintos y la costura se abria (la subdivision la
// tomaba como un borde y las normales no se promediaban)
static std::string W3dmPosQuant(const float* p) {
    int q[3];
    for (int k = 0; k < 3; k++) q[k] = (int)floorf(p[k] * 10000.0f + 0.5f);
    char b[12]; memcpy(b, q, 12); return std::string(b, 12);
}
// LOS ARCHIVOS GUARDADOS ANTES traen como puntos DISTINTOS los lados de una costura y los polos de una esfera UV. Sin
// datos de punto (pesos, shape keys) no hay nada que los distinga: se unen por la posicion cuantizada, y el
// representante sigue siendo el MENOR indice. Con pesos no: dos vertices distintos en el mismo lugar pueden ser de
// huesos distintos. La usan los dos cargadores (el de texto, aca abajo, y el binario: W3dMeshBin.cpp)
struct W3dmUF {
    static int Root(std::vector<int>& u, int x) { while (u[(size_t)x] != x) { u[(size_t)x] = u[(size_t)u[(size_t)x]]; x = u[(size_t)x]; } return x; }
    static void Join(std::vector<int>& u, int a, int b) { a = Root(u, a); b = Root(u, b); if (a == b) return;
                                                         if (a < b) u[(size_t)b] = a; else u[(size_t)a] = b; }
};
void W3dMeshJoinByPosition(Mesh* m) {
    if (!m || !m->vertex || m->vertexSize <= 0 || !m->vertexGroups.empty() || !m->shapeKeys.empty()) return;
    const int nRV = m->vertexSize;
    if ((int)m->posRep.size() != nRV) return;
    std::vector<int> uf((size_t)nRV);
    for (int gi = 0; gi < nRV; gi++) uf[(size_t)gi] = gi;
    std::map<std::string, int> first;
    for (int gi = 0; gi < nRV; gi++) {
        const int r = m->posRep[(size_t)gi];
        if (r >= 0 && r < nRV) W3dmUF::Join(uf, gi, r);
        const std::string kq = W3dmPosQuant(&m->vertex[(size_t)gi * 3]);
        std::map<std::string, int>::iterator it = first.find(kq);
        if (it == first.end()) first[kq] = gi; else W3dmUF::Join(uf, gi, it->second);
    }
    const bool withCp = ((int)m->vertCtrlPoint.size() == nRV);
    for (int gi = 0; gi < nRV; gi++) {
        const int r = W3dmUF::Root(uf, gi);
        m->posRep[(size_t)gi] = r;
        if (withCp) m->vertCtrlPoint[(size_t)gi] = r;
    }
}
static std::string W3dmEdgeKey(const float* a, const float* b) {
    std::string ka = W3dmPosKey(a), kb = W3dmPosKey(b);
    return (ka < kb) ? (ka + kb) : (kb + ka); // el borde no tiene direccion
}

// ---------------------------------------------------------------------------
//  COTA de reserva. NINGUNA cantidad leida del archivo se usa para reservar sin
//  acotarla, y la cota sale del PROPIO TAMANO DEL ARCHIVO (no de una constante
//  inventada): cada campo ocupa como minimo un digito y un separador. Un archivo
//  corrupto que diga "V 2000000000" reserva lo que el archivo fisicamente
//  permite, no 24 GB.
// ---------------------------------------------------------------------------
static int W3dmBound(int declared, size_t bytesThatRemain, int fieldsByElement) {
    if (declared <= 0) return 0;
    if (fieldsByElement < 1) fieldsByElement = 1;
    size_t bound = bytesThatRemain / (size_t)(2 * fieldsByElement);
    if ((size_t)declared > bound) return (int)bound;
    return declared;
}

// ---------------------------------------------------------------------------
//  AVISOS ACOTADOS. Un .w3dm roto de 2 MB puede tener 200.000 lineas malas: la
//  lista de avisos era LO UNICO del lector sin techo y crecia decenas de MB por
//  un archivo que ya sabemos que esta mal. Se guardan los primeros
//  kW3dmMaxNotices y el resto se CUENTA (info->noticesOfMore), asi el techo no
//  esconde nada: el renglon final lo dice.
// ---------------------------------------------------------------------------
static void W3dmNotice(W3dMeshInfo* info, int line, const std::string& txt) {
    if (!info) return;
    if ((int)info->notices.size() >= (int)kW3dmMaxNotices) { info->noticesOfMore++; return; }
    std::string s = "linea ";
    W3dWriteInt(s, line);
    s += ": ";
    s += txt;
    info->notices.push_back(s);
}

// ---------------------------------------------------------------------------
//  "END" TOLERANTE. Comparar los 3 bytes pelados hacia que un espacio INVISIBLE
//  despues del END ("END ") no matcheara: el bloque seguia abierto y se tragaba
//  el resto del archivo (incluido el bloque V) sin un solo aviso. Para un formato
//  que se vende como editable a mano eso es una trampa mortal, asi que se
//  recortan blancos de los dos bordes y se acepta en cualquier caja.
// ---------------------------------------------------------------------------
static bool W3dmIsEnd(const char* li, const char* lf) {
    while (li < lf && (*li == ' ' || *li == '\t')) li++;
    while (lf > li && (*(lf - 1) == ' ' || *(lf - 1) == '\t')) lf--;
    if (lf - li != 3) return false;
    return (li[0] == 'E' || li[0] == 'e') && (li[1] == 'N' || li[1] == 'n') &&
           (li[2] == 'D' || li[2] == 'd');
}

// ---------------------------------------------------------------------------
//  ESCAPE DEL PAYLOAD DE 'PARTS' (ver el comentario grande de W3dMesh.h).
//  PARTS es el UNICO bloque cuyo dato es un nombre pelado, asi que un mesh part
//  llamado "END" emitia una linea que el END TOLERANTE recortaba y aceptaba: el
//  bloque se cerraba de mas, el END verdadero se leia como cabecera mal formada y
//  se comia el bloque PARTMAT entero (la malla perdia las partes Y sus
//  materiales). Se escapa con UN '\' adelante y el lector saca UNO: lossless,
//  ancho 1 igual que siempre, y no hace falta tocar el END tolerante (que es lo
//  que hace que el formato se banque editarse a mano).
// ---------------------------------------------------------------------------
static bool W3dmNameCollidesWithEnd(const std::string& n) {
    const char* a = n.c_str();
    return W3dmIsEnd(a, a + n.size());
}
static std::string W3dmEscapePayload(const std::string& n) {
    if (W3dmNameCollidesWithEnd(n) || (!n.empty() && n[0] == '\\')) return "\\" + n;
    return n;
}
static std::string W3dmUnescapePayload(const std::string& n) {
    if (!n.empty() && n[0] == '\\') return n.substr(1);
    return n;
}

// ---------------------------------------------------------------------------
//  NOMBRES AL ESCRIBIR. El unico caracter que el formato NO puede representar en
//  un nombre es el SALTO DE LINEA: partiria el archivo en dos y la mitad de abajo
//  se leeria como bloques sueltos. El escritor NO puede emitirlo nunca (aunque el
//  nombre venga de un .json editado a mano o de un importador), asi que se
//  reemplaza por un espacio y SE AVISA. Los espacios de los bordes SI se
//  conservan: el lector los respeta (W3dLineName).
// ---------------------------------------------------------------------------
static std::string W3dmSaneName(const std::string& n, std::vector<std::string>* notices,
                                  const char* that) {
    bool dirty = false;
    for (size_t i = 0; i < n.size(); i++)
        if (n[i] == '\n' || n[i] == '\r') { dirty = true; break; }
    if (!dirty) return n;
    std::string s = n;
    for (size_t i = 0; i < s.size(); i++) if (s[i] == '\n' || s[i] == '\r') s[i] = ' ';
    if (notices) {
        std::string a = "el nombre ";
        a += that;
        a += " tenia un salto de linea (partiria el archivo en dos): lo guardo como '";
        a += s; a += "'";
        notices->push_back(a);
    }
    return s;
}
std::string W3dMeshSaneName(const std::string& n, std::vector<std::string>* notices, const char* that) {
    return W3dmSaneName(n, notices, that);
}

// ===========================================================================
// ===========================================================================
//                              E S C R I T U R A
// ===========================================================================
// ===========================================================================

static void CabBlock(std::string& s, const char* name, int count, int domain) {
    s += name; s += ' ';
    W3dWriteInt(s, count); s += ' ';
    s += W3dmDomName(domain);
}

// ---------------------------------------------------------------------------
//  EL FRENO DE MANO (spec seccion 17). Si el archivo dijo "requiere <X>" y X no
//  esta en kBlocks, esta version NO puede interpretar bien la malla: guardar
//  encima la degradaria EN SILENCIO, que es justo lo que esa directiva existe
//  para impedir. Devuelve el nombre del primer bloque que falta ("" = se puede
//  guardar). Se mira la lista GUARDADA, no un flag: asi sigue dando la respuesta
//  correcta cuando una version futura aprenda el bloque.
// ---------------------------------------------------------------------------
std::string W3dMeshMissingBlock(const Mesh* m) {
    if (!m) return std::string();
    for (size_t i = 0; i < m->w3dmForeign.requiresValue.size(); i++)
        if (!W3dMeshBlockKnown(m->w3dmForeign.requiresValue[i])) return m->w3dmForeign.requiresValue[i];
    // el MISMO freno para la malla binaria: su bloque REQ nombro un fourcc que esta version
    // no entiende (ver W3dForeign::requiresBin y io/W3dMeshBin.h)
    for (size_t i = 0; i < m->w3dmForeign.requiresBin.size(); i++)
        if (!W3dMeshBinBlockKnown(m->w3dmForeign.requiresBin[i])) return m->w3dmForeign.requiresBin[i];
    return std::string();
}

// ===========================================================================
//  PIEZAS COMPARTIDAS CON EL .w3db (ver el bloque grande del final de W3dMesh.h).
//  Son pedazos de W3dMeshWrite sacados afuera TAL CUAL, en el mismo orden de
//  avisos: el escritor de texto los llama en el mismo lugar donde estaban.
// ===========================================================================

void W3dMeshComputePoints(const Mesh* m, W3dMeshPoints& out) {
    out.rvToPoint.clear(); out.pts.clear(); out.byPos.clear();
    out.nV = 0; out.nP = 0;
    if (!m) return;
    const int nV = (m->vertex ? m->vertexSize : 0);
    out.nV = nV;

    // ---- PUNTOS DE CONTROL: posiciones UNICAS por BYTES, en orden de aparicion
    //      del array de render. Un cubo se lee "8 vertices", no 24.
    //
    //  CON UNA EXCEPCION: LA GEOMETRIA SUELTA NO SE SUELDA. Dedupear por posicion
    //  esta bien para las esquinas de cara (es lo que hace que el cubo tenga 8
    //  puntos), pero un VERTICE SUELTO parado justo encima de otro es OTRO vertice:
    //  el usuario los ve como dos y GenerateRender NO los une. Si el archivo los
    //  colapsara en un punto, "agregar vertice / Shift+D / Esc" seguido de guardar
    //  perderia uno SIN AVISO, y eso no es "lo que ya hacia el editor": es topologia
    //  que cambia el formato. Entonces los render-verts SUELTOS (looseVerts y los
    //  extremos de looseEdges que no usa ninguna cara) se llevan un punto PROPIO;
    //  render-verts sueltos que SI son el mismo (dos aristas sueltas encadenadas)
    //  comparten punto, porque comparten indice de render.
    std::vector<int>&  rvToPoint = out.rvToPoint;
    rvToPoint.assign((size_t)(nV > 0 ? nV : 1), -1);
    std::vector<char>  rvOfFace((size_t)(nV > 0 ? nV : 1), 0);
    std::vector<char>  rvLoose((size_t)(nV > 0 ? nV : 1), 0);
    for (size_t f = 0; f < m->faces3d.size(); f++)
        for (size_t c = 0; c < m->faces3d[f].idx.size(); c++) {
            int gv = m->faces3d[f].idx[c];
            if (gv >= 0 && gv < nV) rvOfFace[(size_t)gv] = 1;
        }
    for (size_t i = 0; i < m->looseEdges.size(); i++) {
        int gv = m->looseEdges[i];
        if (gv >= 0 && gv < nV && !rvOfFace[(size_t)gv]) rvLoose[(size_t)gv] = 1;
    }
    for (size_t i = 0; i < m->looseVerts.size(); i++) {
        int gv = m->looseVerts[i];
        if (gv >= 0 && gv < nV && !rvOfFace[(size_t)gv]) rvLoose[(size_t)gv] = 1;
    }
    // ---- LOS DATOS DEL PUNTO tambien lo identifican. Dos render-verts en la MISMA
    //      posicion son el mismo punto solo si ademas tienen los MISMOS datos de punto
    //      (pesos de cada vertex group, deltas de cada shape key). Una costura de UV o de
    //      color los tiene iguales (es el mismo vertice partido) y se une como siempre;
    //      pero dos vertices DISTINTOS que solo coinciden en el lugar (el borde de una
    //      puerta pegado a la carroceria, cada uno de su hueso) NO son el mismo: soldarlos
    //      al guardar mezclaba sus pesos y la malla se estiraba al animar. Guardar nunca
    //      suelda lo que la malla no tiene soldado.
    std::vector<std::string> signatureRv((size_t)(nV > 0 ? nV : 1));
    if (nV > 0 && (!m->vertexGroups.empty() || !m->shapeKeys.empty())) {
        const bool withCp = ((int)m->vertCtrlPoint.size() == nV);
        std::map<int, std::string> signatureCp;   // control-point -> sus datos (bytes)
        for (size_t g = 0; g < m->vertexGroups.size(); g++) {
            const VertexGroup* vg = m->vertexGroups[g];
            if (!vg) continue;
            for (size_t k = 0; k < vg->verts.size() && k < vg->weights.size(); k++) {
                if (vg->weights[k] == 0.0f) continue;          // peso 0 = no estar en el grupo
                char b[8]; int gi = (int)g; float w = vg->weights[k];
                memcpy(b, &gi, 4); memcpy(b + 4, &w, 4);
                signatureCp[vg->verts[k]].append(b, 8);
            }
        }
        for (size_t g = 0; g < m->shapeKeys.size(); g++) {
            const W3dShapeKey& sk = m->shapeKeys[g];
            for (size_t j = 0; j < sk.idx.size() && j * 3 + 2 < sk.d.size(); j++) {
                char b[17]; int gi = (int)g; b[0] = 'S';
                memcpy(b + 1, &gi, 4); memcpy(b + 5, &sk.d[j * 3], 12);
                signatureCp[sk.idx[j]].append(b, 17);
            }
        }
        for (int i = 0; i < nV; i++) {
            int cp = withCp ? m->vertCtrlPoint[(size_t)i] : i;
            std::map<int, std::string>::iterator it = signatureCp.find(cp);
            if (it != signatureCp.end()) signatureRv[(size_t)i] = it->second;
        }
    }
    std::vector<float>& pts = out.pts;
    // posicion -> TODOS los puntos parados ahi, de menor a mayor (sharp/seam se indexan por
    // posicion). Casi siempre es uno solo; son varios cuando la malla tiene vertices distintos
    // en el mismo lugar (los de arriba, con otros pesos) o un punto suelto encima de una cara.
    std::map<std::string, std::vector<int> >& mapP = out.byPos;
    std::map<std::string, int> mapPD;     // posicion + datos del punto -> punto
    pts.reserve((size_t)nV * 3);
    for (int i = 0; i < nV; i++) {
        std::string k = W3dmPosKey(&m->vertex[i * 3]);
        std::string kd = W3dmPosQuant(&m->vertex[i * 3]) + signatureRv[(size_t)i];
        if (!rvLoose[(size_t)i]) {
            std::map<std::string, int>::iterator it = mapPD.find(kd);
            if (it != mapPD.end()) {
                rvToPoint[(size_t)i] = it->second;
                // (su posicion EXACTA tambien resuelve al punto: sharp/seam se indexan por los bits de cada lado)
                std::vector<int>& ps = mapP[k];
                bool already = false; for (size_t q = 0; q < ps.size() && !already; q++) already = (ps[q] == it->second);
                if (!already) ps.push_back(it->second);
                continue;
            }
        }
        int pi = (int)(pts.size() / 3);
        pts.push_back(m->vertex[i * 3]);
        pts.push_back(m->vertex[i * 3 + 1]);
        pts.push_back(m->vertex[i * 3 + 2]);
        // TODO punto nuevo entra al mapa de POSICION (tambien el suelto): sharp/seam se
        // indexan por posicion y tienen que poder resolverla contra el punto de SU arista.
        mapP[k].push_back(pi);
        if (mapPD.find(kd) == mapPD.end()) mapPD[kd] = pi;
        rvToPoint[(size_t)i] = pi;
    }
    out.nP = (int)(pts.size() / 3);
}

void W3dMeshPickLayers(const Mesh* m, int nC, W3dMeshLayersExit& out, std::vector<std::string>* notices) {
    out.uv.clear(); out.col.clear();
    out.uvActive = -1; out.activeCol = -1; out.uvDerived = false; out.colDerived = false;
    if (!m) return;
    const int nV = (m->vertex ? m->vertexSize : 0);
    // CAPAS LAZY (ver el comentario grande del bloque NRM): si la malla todavia no
    // materializo sus capas, el archivo lleva igual la version derivada del render y la
    // capa 0 es la activa.
    out.uvDerived = (m->uvMaps.empty() && m->uv && nC > 0 && nV > 0);
    if (m->colorLayers.empty() && m->vertexColor && nC > 0 && nV > 0)
        for (int i = 0; i < nV * 4 && !out.colDerived; i++) if (m->vertexColor[i] != 255) out.colDerived = true;

    // ---- QUE CAPAS SALEN, DECIDIDO ANTES DE ESCRIBIR NADA ------------------
    //  Una capa que no mide lo que la topologia dice NO se puede escribir (el
    //  bloque es POR ESQUINA), pero saltearla en silencio rompia DOS promesas:
    //  la del header ("si algo no sobrevive, se dice") y, peor, la del indice:
    //  "uvactiva"/"colactiva" se escriben ARRIBA con el indice de la lista
    //  COMPLETA, asi que saltear una capa ANTERIOR a la activa dejaba el indice
    //  apuntando a OTRA capa y la malla volvia con otro uv/color en el render.
    //  Por eso el indice que se escribe es el de la lista REALMENTE escrita.
    std::vector<size_t>& uvExit = out.uv;
    std::vector<size_t>& colExit = out.col;
    for (size_t l = 0; l < m->uvMaps.size(); l++) {
        const UVMap* u = m->uvMaps[l];
        if (!u || (int)u->uv.size() != nC * 2) {
            if (notices) {
                std::string s = "no pude guardar la capa UV '";
                s += (u ? u->name : std::string("?"));
                s += "': tiene "; W3dWriteInt(s, u ? (int)(u->uv.size() / 2) : 0);
                s += " esquinas y la malla tiene "; W3dWriteInt(s, nC);
                notices->push_back(s);
            }
            continue;
        }
        uvExit.push_back(l);
    }
    for (size_t l = 0; l < m->colorLayers.size(); l++) {
        const ColorLayer* c = m->colorLayers[l];
        if (!c || (int)c->color.size() != nC * 4) {
            if (notices) {
                std::string s = "no pude guardar la capa de color '";
                s += (c ? c->name : std::string("?"));
                s += "': tiene "; W3dWriteInt(s, c ? (int)(c->color.size() / 4) : 0);
                s += " esquinas y la malla tiene "; W3dWriteInt(s, nC);
                notices->push_back(s);
            }
            continue;
        }
        colExit.push_back(l);
    }
    int uvActiveSale = -1, activeColSale = -1;
    for (size_t k = 0; k < uvExit.size(); k++)  if ((int)uvExit[k]  == m->uvActiveMap) uvActiveSale  = (int)k;
    for (size_t k = 0; k < colExit.size(); k++) if ((int)colExit[k] == m->activeColor) activeColSale = (int)k;
    // un indice FUERA DE RANGO (estado inconsistente en memoria, no una capa
    // descartada) cae a la capa 0: es exactamente lo que hacia el lector al
    // clampearlo, y ese comportamiento no se toca.
    if (m->uvActiveMap  >= (int)m->uvMaps.size()      && !uvExit.empty())  uvActiveSale  = 0;
    if (m->activeColor  >= (int)m->colorLayers.size() && !colExit.empty()) activeColSale = 0;
    if (notices && m->uvActiveMap >= 0 && m->uvActiveMap < (int)m->uvMaps.size() && uvActiveSale < 0)
        notices->push_back("la capa UV ACTIVA no se pudo guardar: el archivo queda sin capa UV activa");
    if (notices && m->activeColor >= 0 && m->activeColor < (int)m->colorLayers.size() && activeColSale < 0)
        notices->push_back("la capa de color ACTIVA no se pudo guardar: el archivo queda sin capa de color activa");
    out.uvActive = uvActiveSale;
    out.activeCol = activeColSale;
}

void W3dMeshRealEdges(const Mesh* m, const W3dMeshPoints& P, std::set<std::pair<int, int> >& out) {
    out.clear();
    if (!m) return;
    const int nV = P.nV;
    const std::vector<int>& rvToPoint = P.rvToPoint;
    for (size_t f = 0; f < m->faces3d.size(); f++) {
        const MeshFace& F = m->faces3d[f];
        for (size_t c = 0; c < F.idx.size(); c++) {
            int ga = F.idx[c], gb = F.idx[(c + 1) % F.idx.size()];
            if (ga < 0 || ga >= nV || gb < 0 || gb >= nV) continue;
            int a = rvToPoint[(size_t)ga], b = rvToPoint[(size_t)gb];
            if (a == b) continue;
            out.insert(a < b ? std::make_pair(a, b) : std::make_pair(b, a));
        }
    }
    for (size_t i = 0; i + 1 < m->looseEdges.size(); i += 2) {
        int ga = m->looseEdges[i], gb = m->looseEdges[i + 1];
        if (ga < 0 || ga >= nV || gb < 0 || gb >= nV) continue;
        int a = rvToPoint[(size_t)ga], b = rvToPoint[(size_t)gb];
        if (a == b) continue;
        out.insert(a < b ? std::make_pair(a, b) : std::make_pair(b, a));
    }
}

void W3dMeshMarksValid(const Mesh* m, const W3dMeshPoints& P, const std::set<std::pair<int, int> >& realEdges,
                           bool seam, std::vector<std::pair<int, int> >& pairs, std::vector<std::string>* notices) {
    pairs.clear();
    if (!m) return;
    const std::set<std::string>& src = seam ? m->seamEdges : m->sharpEdges;
    if (src.empty()) return;
    const int nV = P.nV;
    // INVARIANTE DE POSE (el tercero de la clase; el mismo que respeta la poda,
    // ver Mesh::PruneMarksNoEdge y Mesh::RestPositions): las claves de
    // sharp/seam viven en el espacio de REPOSO. Con la malla POSADA por una vertex
    // anim, 'mapP' se armo con las posiciones DE LA POSE y NINGUNA clave resuelve
    // -> el escritor las daba TODAS por perdidas y el archivo salia sin UNA SOLA
    // marca. Ese es, literal, el sintoma que reporto el dueno: marcar, mover el
    // playhead, guardar. Aca el mapa de busqueda se rearma con las posiciones del
    // KEYFRAME BASE; rvToPoint (render-vert -> punto) NO depende de la pose, asi que
    // los indices que se escriben son exactamente los mismos.
    // (El guardado del proyecto ademas deja la malla en reposo antes de llamar
    //  -W3dRestVertexAnim-, que es lo que evita ademas hornear la pose como
    //  geometria; esto cierra el camino directo al escritor.)
    std::map<std::string, std::vector<int> > mapRest;
    if (m->PosedByVertexAnim()) {
        const GLfloat* R = m->RestPositions();
        if (R && R != m->vertex)
            for (int i = 0; i < nV; i++) {
                const int p = P.rvToPoint[(size_t)i];
                if (p < 0) continue;
                std::vector<int>& list = mapRest[W3dmPosKey(&R[i * 3])];
                if (std::find(list.begin(), list.end(), p) == list.end()) list.push_back(p);
            }
    }
    const std::map<std::string, std::vector<int> >& mapKey = mapRest.empty() ? P.byPos : mapRest;
    pairs.reserve(src.size());
    int lost = 0;
    for (std::set<std::string>::const_iterator it = src.begin(); it != src.end(); ++it) {
        if (it->size() != 24) { lost++; continue; }
        std::map<std::string, std::vector<int> >::const_iterator a = mapKey.find(it->substr(0, 12));
        std::map<std::string, std::vector<int> >::const_iterator b = mapKey.find(it->substr(12, 12));
        // (1) la clave de POSICION ya no cae en ningun punto de la malla: el vertice
        //     se MOVIO o se BORRO. SILENCIOSO JAMAS: perderla esta bien, perderla sin
        //     decirlo no (y el aviso no puede afirmar CUAL de las dos fue).
        if (a == mapKey.end() || b == mapKey.end()) { lost++; continue; }
        // (2) los puntos existen PERO no hay una arista entre ellos: es la variante
        //     FANTASMA (borrar caras deja los puntos vivos, o un corte parte la arista
        //     en dos). Validar solo las POSICIONES no alcanzaba y la marca se escribia
        //     sobre una arista que no esta.
        //     Con VARIOS puntos en una posicion (vertices distintos en el mismo lugar)
        //     la marca es de TODA arista real entre esas dos posiciones, igual que en
        //     memoria (la clave es de posicion): tomar solo el primer punto perdia la
        //     costura de la arista que usa al otro, y el archivo salia sin ella.
        bool any = false;
        for (size_t ia = 0; ia < a->second.size(); ia++)
            for (size_t ib = 0; ib < b->second.size(); ib++) {
                int x = a->second[ia], y = b->second[ib];
                if (x == y) continue;
                if (x > y) { int t = x; x = y; y = t; }
                if (!realEdges.count(std::make_pair(x, y))) continue;
                pairs.push_back(std::make_pair(x, y));
                any = true;
            }
        if (!any) lost++;
    }
    if (lost > 0 && notices) {
        std::string s = "no pude guardar "; W3dWriteInt(s, lost);
        s += (seam ? " costura(s) SEAM" : " borde(s) SHARP");
        s += ": esa arista ya no existe en la malla (se borro o se movio la geometria despues de marcarla)";
        notices->push_back(s);
    }
    // orden numerico estable (el set viene ordenado por BYTES de float, que
    // no es un orden util para un humano ni estable ante un re-merge).
    // std::sort y no el doble for de antes: era O(n^2) y con 80.000 aristas
    // marcadas se comia varios segundos EN CADA GUARDADO.
    std::sort(pairs.begin(), pairs.end());
    // la misma arista no se escribe dos veces (una clave (A,B) y otra (B,A) del set
    // llegarian al mismo par): el lector la volveria a juntar y re-guardar cambiaria bytes
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
}

void W3dMeshCpRep(const Mesh* m, std::map<int, int>& cpRep) {
    cpRep.clear();
    if (!m) return;
    const int nV = (m->vertex ? m->vertexSize : 0);
    if ((int)m->vertCtrlPoint.size() == nV)
        for (int i = 0; i < nV; i++) {
            int cp = m->vertCtrlPoint[(size_t)i];
            if (cp >= 0 && cpRep.find(cp) == cpRep.end()) cpRep[cp] = i;
        }
}

bool W3dMeshGroupByPoint(const Mesh* m, const W3dMeshPoints& P, const std::map<int, int>& cpRep,
                           size_t g, std::vector<int>& pi, std::vector<float>& pw, std::vector<std::string>* notices) {
    pi.clear(); pw.clear();
    if (!m || g >= m->vertexGroups.size() || !m->vertexGroups[g]) return false;
    const VertexGroup* vg = m->vertexGroups[g];
    const int nV = P.nV;
    // control-point -> punto: el control-point es un indice de render-vert
    // representante (vertCtrlPoint), asi que se traduce por su posicion.
    std::set<int> seen;
    int collisions = 0;
    for (size_t k = 0; k < vg->verts.size() && k < vg->weights.size(); k++) {
        int cp = vg->verts[k];
        int rv = -1;
        if (!cpRep.empty()) {
            std::map<int, int>::const_iterator it = cpRep.find(cp);
            if (it != cpRep.end()) rv = it->second;
        } else if (cp >= 0 && cp < nV) rv = cp;
        if (rv < 0) continue;
        int p = P.rvToPoint[(size_t)rv];
        if (p < 0) continue;
        // DOS CONTROL-POINTS EN LA MISMA POSICION: en el archivo son UN punto.
        // Es el caso NORMAL de una costura de UV o de color (el vertice se parte
        // pero la posicion es una sola) y ahi los dos pesos son el MISMO valor:
        // no se pierde nada, el punto ya lo representa. Solo hay perdida real
        // cuando los pesos DIFIEREN -- eso pasa con una malla importada de
        // glTF/FBX, donde el control-point lo trae el importador. Gana el
        // primero; perderlo esta bien, perderlo EN SILENCIO no.
        if (!seen.insert(p).second) {
            float already = 0.0f;
            for (size_t q = 0; q < pi.size(); q++) if (pi[q] == p) { already = pw[q]; break; }
            const float d = already - vg->weights[k];
            if (d > 1e-6f || d < -1e-6f) collisions++;
            continue;
        }
        pi.push_back(p); pw.push_back(vg->weights[k]);
    }
    if (collisions > 0 && notices) {
        std::string s = "el vertex group '"; s += vg->name;
        s += "' perdio "; W3dWriteInt(s, collisions);
        s += " peso(s): son control-points distintos parados en la MISMA posicion y en el archivo son un solo punto";
        notices->push_back(s);
    }
    return true;
}

void W3dMeshShapeByPoint(const Mesh* m, const W3dMeshPoints& P, const std::map<int, int>& cpRep,
                           size_t k, std::vector<int>& pi, std::vector<float>& pd) {
    pi.clear(); pd.clear();
    if (!m || k >= m->shapeKeys.size()) return;
    const W3dShapeKey& sk = m->shapeKeys[k];
    const int nV = P.nV;
    std::set<int> seen;
    for (size_t j = 0; j < sk.idx.size() && j * 3 + 2 < sk.d.size(); j++) {
        int cp = sk.idx[j], rv = -1;
        if (!cpRep.empty()) { std::map<int, int>::const_iterator it = cpRep.find(cp); if (it != cpRep.end()) rv = it->second; }
        else if (cp >= 0 && cp < nV) rv = cp;
        if (rv < 0) continue;
        int pt = P.rvToPoint[(size_t)rv];
        if (pt < 0 || !seen.insert(pt).second) continue;   // mismo punto (costura): el delta es el mismo
        pi.push_back(pt); pd.push_back(sk.d[j*3]); pd.push_back(sk.d[j*3+1]); pd.push_back(sk.d[j*3+2]);
    }
}

bool W3dMeshUVGroupByCorner(const Mesh* m, size_t g, std::vector<int>& ci, std::vector<float>& cw) {
    ci.clear(); cw.clear();
    if (!m || g >= m->uvGroups.size() || !m->uvGroups[g]) return false;
    const UVGroup* ug = m->uvGroups[g];
    const int nV = (m->vertex ? m->vertexSize : 0);
    std::vector<float> w((size_t)(nV > 0 ? nV : 1), 0.0f);
    for (size_t k = 0; k < ug->verts.size() && k < ug->weights.size(); k++) {
        int rv = ug->verts[k];
        if (rv >= 0 && rv < nV) w[(size_t)rv] = ug->weights[k];
    }
    int L = 0;
    for (size_t f = 0; f < m->faces3d.size(); f++)
        for (size_t c = 0; c < m->faces3d[f].idx.size(); c++, L++) {
            int gv = m->faces3d[f].idx[c];
            if (gv < 0 || gv >= nV) continue;
            if (w[(size_t)gv] > 0.0f) { ci.push_back(L); cw.push_back(w[(size_t)gv]); }
        }
    return true;
}

bool W3dMeshWrite(const Mesh* m, std::string& out, std::vector<std::string>* notices, int anchorRender) {
    out.clear();
    if (!m) return false;
    // NO SE GUARDA ENCIMA DE UNA MALLA QUE NO ENTENDEMOS. El que llama ya deberia
    // haber avisado con el nombre del bloque (W3dMeshMissingBlock); esto es el
    // cierre, para que ningun camino nuevo se saltee el freno por olvido.
    if (!W3dMeshMissingBlock(m).empty()) {
        if (notices)
            notices->push_back("no guardo esta malla: el archivo REQUIERE el bloque '" +
                              W3dMeshMissingBlock(m) + "' y esta version no lo entiende");
        return false;
    }

    // ---- PUNTOS DE CONTROL: la IDENTIDAD de los vertices (ver W3dMeshComputePoints, que
    //      comparte con el .w3db): posicion + datos de punto; la geometria suelta no se suelda.
    W3dMeshPoints P;
    W3dMeshComputePoints(m, P);
    const int nV = P.nV;
    const std::vector<int>& rvToPoint = P.rvToPoint;
    const std::vector<float>& pts = P.pts;
    const int nP = P.nP;

    // ---- esquinas (indice de TODAS las capas por esquina)
    int nC = 0;
    for (size_t f = 0; f < m->faces3d.size(); f++) nC += (int)m->faces3d[f].idx.size();
    const int nF = (int)m->faces3d.size();

    // ---- CABECERA + DIRECTIVAS ------------------------------------------
    out += "W3DMESH 1\n";
    out += "# malla de Whisk3D. texto por lineas, indices base 0.\n";
    out += "# MAYUSCULA abre bloque (cierra END) | minuscula = directiva | espacio = dato\n";
    // FRENO DE MANO: sin ENTENDER estos bloques el archivo no se puede interpretar.
    // Un lector que vea aca un nombre que no conoce abre en SOLO LECTURA. Tiene que
    // existir desde la version 1: agregarla despues NO sirve (los lectores viejos la
    // ignorarian como cualquier directiva desconocida).
    //
    // SE RE-EMITE TAL CUAL VINO. Antes esta linea se escribia a mano siempre igual
    // ("requiere V F") y por eso la directiva se AUTO-BORRABA: un archivo con
    // "requiere V F CREASE" abierto y guardado por un Whisk3D que no conoce CREASE
    // salia con la valvula de menos, el bloque CREASE preservado pero YA SIN NADIE
    // QUE LO PROTEJA, y el guardado siguiente lo tiraba en silencio. Justo el
    // degradado que 'requiere' existe para evitar. Ahora la lista viaja con la
    // malla (W3dForeign::requiere) y solo se pone el default cuando no salio de un
    // archivo (malla nueva del editor).
    if (!m->w3dmForeign.requiresValue.empty()) {
        out += "requires";
        for (size_t i = 0; i < m->w3dmForeign.requiresValue.size(); i++) {
            out += ' ';
            out += W3dmSaneName(m->w3dmForeign.requiresValue[i], notices, "de un bloque de 'requiere'");
        }
        out += "\n";
    } else {
        out += "requires V F\n";
    }
    out += "name "; out += W3dmSaneName(m->name, notices, "de la malla"); out += "\n";
    out += "points ";   W3dWriteInt(out, nP); out += "\n";
    out += "corners "; W3dWriteInt(out, nC); out += "\n";
    out += "faces ";    W3dWriteInt(out, nF); out += "\n";
    // rverts: solo tiene sentido cuando hay que ANCLAR indices de render (vertex anims)
    // (anchorRender: -1 = si la malla tiene vertex anims; 0/1 = lo decide el que llama, ver el .h)
    const bool withAnim = (anchorRender < 0) ? !m->animations.empty() : (anchorRender != 0);
    if (withAnim) { out += "rverts "; W3dWriteInt(out, nV); out += "\n"; }
    out += "smooth ";    W3dWriteInt(out, m->meshSmooth ? 1 : 0); out += "\n";
    // QUE CAPAS SALEN (y las LAZY: la derivada del render), decidido ANTES de escribir nada.
    // Ver W3dMeshPickLayers: el mismo criterio (y los mismos avisos) que usa el .w3db.
    W3dMeshLayersExit layers;
    W3dMeshPickLayers(m, nC, layers, notices);
    const bool uvDerived = layers.uvDerived, colDerived = layers.colDerived;
    const std::vector<size_t>& uvExit = layers.uv;
    const std::vector<size_t>& colExit = layers.col;
    const int uvActiveSale = layers.uvActive, activeColSale = layers.activeCol;

    if (!m->uvMaps.empty())       { out += "activeuv ";   W3dWriteInt(out, uvActiveSale);  out += "\n"; }
    else if (uvDerived)          { out += "activeuv 0\n"; }
    if (!m->colorLayers.empty())  { out += "activecol ";  W3dWriteInt(out, activeColSale);  out += "\n"; }
    else if (colDerived)         { out += "activecol 0\n"; }
    if (!m->vertexGroups.empty()) { out += "activevg ";   W3dWriteInt(out, m->activeGroup);  out += "\n"; }
    if (!m->uvGroups.empty())     { out += "activeuvg ";  W3dWriteInt(out, m->uvActiveGroup); out += "\n"; }
    if (m->meshType >= 0) {
        out += "prim ";
        if (m->meshType < kPrimN) out += kPrim[m->meshType]; else W3dWriteInt(out, m->meshType);
        out += ' '; W3dWriteFloat(out, m->meshSize);
        out += ' '; W3dWriteFloat(out, m->meshSize2);
        out += ' '; W3dWriteFloat(out, m->meshDepth);
        out += ' '; W3dWriteInt(out, m->meshVerts);
        out += ' '; W3dWriteInt(out, m->meshVerts2);
        out += "\n";
    }

    // ---- V: los PUNTOS de control ---------------------------------------
    out += "\n# los PUNTOS de control (x y z)\n";
    CabBlock(out, "V", nP, W3DM_POINT); out += "\n";
    for (int i = 0; i < nP; i++) {
        out += ' '; W3dWriteFloat(out, pts[(size_t)i * 3]);
        out += ' '; W3dWriteFloat(out, pts[(size_t)i * 3 + 1]);
        out += ' '; W3dWriteFloat(out, pts[(size_t)i * 3 + 2]);
        out += "\n";
    }
    out += "END\n";

    // ---- F: caras NATIVAS (tri / quad / ngon, sin triangular) -----------
    out += "\n# caras: <lados> y sus puntos, en orden alrededor del poligono\n";
    CabBlock(out, "F", nF, W3DM_FACE); out += "\n";
    for (int f = 0; f < nF; f++) {
        const MeshFace& F = m->faces3d[(size_t)f];
        out += ' '; W3dWriteInt(out, (int)F.idx.size());
        for (size_t c = 0; c < F.idx.size(); c++) {
            int gv = F.idx[c];
            int pi = (gv >= 0 && gv < nV) ? rvToPoint[(size_t)gv] : -1;
            out += ' '; W3dWriteInt(out, pi);
        }
        out += "\n";
    }
    out += "END\n";

    // ---- FPART / FSHADE: SPARSE (lo que no aporta nada NO se escribe) ----
    {
        int n = 0;
        for (int f = 0; f < nF; f++) if (m->faces3d[(size_t)f].mat != 0) n++;
        if (n > 0) {
            out += "\n"; CabBlock(out, "FPART", n, W3DM_FACE); out += "\n";
            for (int f = 0; f < nF; f++) if (m->faces3d[(size_t)f].mat != 0) {
                out += ' '; W3dWriteInt(out, f);
                out += ' '; W3dWriteInt(out, m->faces3d[(size_t)f].mat); out += "\n";
            }
            out += "END\n";
        }
    }
    {
        int n = 0;
        for (int f = 0; f < nF; f++) if (m->faces3d[(size_t)f].smooth >= 0) n++;
        if (n > 0) {
            out += "\n"; CabBlock(out, "FSHADE", n, W3DM_FACE); out += "\n";
            for (int f = 0; f < nF; f++) if (m->faces3d[(size_t)f].smooth >= 0) {
                out += ' '; W3dWriteInt(out, f);
                out += ' '; W3dWriteInt(out, m->faces3d[(size_t)f].smooth); out += "\n";
            }
            out += "END\n";
        }
    }

    // ---- NRM: normal por esquina (dato DEL USUARIO, no derivado) --------
    //  LAS CAPAS SON LAZY EN MEMORIA: una malla recien creada (o importada) todavia NO tiene
    //  cornerNormal ni uvMaps ni colorLayers; sus datos viven solo en los arrays de RENDER hasta
    //  que alguien entra al editor y PopulateLayers() los materializa. El archivo NO puede depender
    //  de eso: si no hay capa se emite la version DERIVADA del render (corner L -> faces3d[f].idx[c],
    //  que es exactamente el mapeo que usa PopulateLayers). Sin esto, guardar un cubo recien agregado
    //  perdia sus UV y sus normales, y al reabrir el merge colapsaba los 24 render-verts en 8.
    //  Al leer, esa version derivada vuelve como una capa de verdad -> el re-guardado es identico.
    if (nC > 0 && ((int)m->cornerNormal.size() == nC * 3 || m->normals)) {
        const bool byLayer = ((int)m->cornerNormal.size() == nC * 3);
        out += "\n# normal por esquina (x y z enteros, escala 1/127)\n";
        CabBlock(out, "NRM", nC, W3DM_CORNER); out += "\n";
        int L = 0;
        for (size_t f = 0; f < m->faces3d.size(); f++)
            for (size_t c = 0; c < m->faces3d[f].idx.size(); c++, L++) {
                int gv = m->faces3d[f].idx[c];
                const GLbyte* n3;
                GLbyte zero[3] = { 0, 127, 0 };
                if (byLayer) n3 = &m->cornerNormal[(size_t)L * 3];
                else if (gv >= 0 && gv < nV) n3 = &m->normals[(size_t)gv * 3];
                else n3 = zero;
                out += ' '; W3dWriteInt(out, (int)n3[0]);
                out += ' '; W3dWriteInt(out, (int)n3[1]);
                out += ' '; W3dWriteInt(out, (int)n3[2]);
                out += "\n";
            }
        out += "END\n";
    }

    // ---- UV: UNA por capa, en el orden de Mesh::uvMaps ------------------
    if (!m->uvMaps.empty()) {
        for (size_t k = 0; k < uvExit.size(); k++) {
            const UVMap* u = m->uvMaps[uvExit[k]];
            out += "\n"; CabBlock(out, "UV", nC, W3DM_CORNER);
            out += " name="; out += W3dmSaneName(u->name, notices, "de una capa UV"); out += "\n";
            for (int i = 0; i < nC; i++) {
                out += ' '; W3dWriteFloat(out, u->uv[(size_t)i * 2]);
                out += ' '; W3dWriteFloat(out, u->uv[(size_t)i * 2 + 1]);
                out += "\n";
            }
            out += "END\n";
        }
    } else if (uvDerived) {
        // sin capa todavia: la UV derivada del render, con el MISMO nombre que le pondria
        // PopulateLayers ("UVMap"), para que leer y volver a guardar de los mismos bytes.
        out += "\n"; CabBlock(out, "UV", nC, W3DM_CORNER); out += " name=UVMap\n";
        for (size_t f = 0; f < m->faces3d.size(); f++)
            for (size_t c = 0; c < m->faces3d[f].idx.size(); c++) {
                int gv = m->faces3d[f].idx[c];
                float u0 = 0.0f, v0 = 0.0f;
                if (gv >= 0 && gv < nV) { u0 = m->uv[(size_t)gv * 2]; v0 = m->uv[(size_t)gv * 2 + 1]; }
                out += ' '; W3dWriteFloat(out, u0);
                out += ' '; W3dWriteFloat(out, v0);
                out += "\n";
            }
        out += "END\n";
    }

    // ---- COL: capas de COLOR POR VERTICE. HOY ESTO SE PIERDE ENTERO -----
    //      (el exportador GLB nunca emitio COLOR_0 y el JSON no las guarda).
    //      El almacenamiento es SIEMPRE por esquina aunque el flag perVertex
    //      este puesto, porque el toggle del editor es NO DESTRUCTIVO: en el
    //      archivo el flag es una CLAVE, no un cambio de layout.
    for (size_t k = 0; k < colExit.size(); k++) {
        const ColorLayer* c = m->colorLayers[colExit[k]];
        out += "\n"; CabBlock(out, "COL", nC, W3DM_CORNER);
        if (c->perVertex) out += " by=vertex";
        out += " name="; out += W3dmSaneName(c->name, notices, "de una capa de color"); out += "\n";
        for (int i = 0; i < nC; i++) {
            out += ' '; W3dWriteInt(out, (int)c->color[(size_t)i * 4]);
            out += ' '; W3dWriteInt(out, (int)c->color[(size_t)i * 4 + 1]);
            out += ' '; W3dWriteInt(out, (int)c->color[(size_t)i * 4 + 2]);
            out += ' '; W3dWriteInt(out, (int)c->color[(size_t)i * 4 + 3]);
            out += "\n";
        }
        out += "END\n";
    }
    if (colDerived) {
        // idem UV: el color por vertice de un .obj importado vive solo en el array de render
        // hasta que alguien abre el editor. Nombre "Col", el mismo que le pondria PopulateLayers.
        // Todo blanco NO se escribe: no aporta nada y engordaria todos los archivos.
        out += "\n"; CabBlock(out, "COL", nC, W3DM_CORNER); out += " name=Col\n";
        for (size_t f = 0; f < m->faces3d.size(); f++)
            for (size_t c = 0; c < m->faces3d[f].idx.size(); c++) {
                int gv = m->faces3d[f].idx[c];
                for (int k = 0; k < 4; k++) {
                    int v = (gv >= 0 && gv < nV) ? (int)m->vertexColor[(size_t)gv * 4 + (size_t)k] : 255;
                    out += ' '; W3dWriteInt(out, v);
                }
                out += "\n";
            }
        out += "END\n";
    }

    // ---- UVREST: base del skinning 2D. En memoria es POR RENDER-VERT; en el
    //      archivo va POR ESQUINA (replicado) y se colapsa al leer. Es lossless
    //      porque todas las esquinas de un mismo render-vert comparten el valor
    //      por construccion.
    //  Se escribe SIEMPRE que el rest sea valido (no solo con armature 2D
    //  presente): el armature vive en el project.json y no viaja en el .w3dm,
    //  asi que condicionarlo a el haria que el rest se perdiera al copiar la
    //  malla sola, y ademas rompe el round-trip byte a byte.
    if ((int)m->uv2dRest.size() == nV * 2 && nV > 0 && nC > 0) {
        out += "\n# UV de REST del skinning 2D (por esquina; se colapsa al leer)\n";
        CabBlock(out, "UVREST", nC, W3DM_CORNER); out += "\n";
        for (size_t f = 0; f < m->faces3d.size(); f++)
            for (size_t c = 0; c < m->faces3d[f].idx.size(); c++) {
                int gv = m->faces3d[f].idx[c];
                float u = 0.0f, v = 0.0f;
                if (gv >= 0 && gv < nV) { u = m->uv2dRest[(size_t)gv * 2]; v = m->uv2dRest[(size_t)gv * 2 + 1]; }
                out += ' '; W3dWriteFloat(out, u);
                out += ' '; W3dWriteFloat(out, v);
                out += "\n";
            }
        out += "END\n";
    }

    // ---- ARISTAS REALES (pares de PUNTOS, sin repetir) de F + EDGE. Las usan
    //      dos cosas: la validacion de SHARP/SEAM (abajo) y el conteo del dominio
    //      W3DM_EDGE de los bloques ajenos (mas abajo). Se arma UNA sola vez y
    //      solo si alguna de las dos la necesita.
    std::set<std::pair<int, int> > realEdges;
    const bool hasMarks = (!m->sharpEdges.empty() || !m->seamEdges.empty());
    if (hasMarks || !m->w3dmForeign.blocks.empty()) W3dMeshRealEdges(m, P, realEdges);

    // ---- SHARP / SEAM: en memoria son sets con clave de POSICION (no hay
    //      indice de arista en ningun lado); en el archivo van como PAR DE
    //      PUNTOS y al cargar se rearma la clave desde las posiciones leidas.
    //      La validacion (y el invariante de POSE) vive en W3dMeshMarksValid.
    for (int pass = 0; pass < 2; pass++) {
        std::vector<std::pair<int, int> > pairs;
        W3dMeshMarksValid(m, P, realEdges, pass != 0, pairs, notices);
        if (pairs.empty()) continue;
        out += "\n"; CabBlock(out, pass ? "SEAM" : "SHARP", (int)pairs.size(), W3DM_EDGE); out += "\n";
        for (size_t i = 0; i < pairs.size(); i++) {
            out += ' '; W3dWriteInt(out, pairs[i].first);
            out += ' '; W3dWriteInt(out, pairs[i].second); out += "\n";
        }
        out += "END\n";
    }

    // ---- EDGE / POINT: geometria SUELTA (hoy se pierde entera).
    //      EL ORDEN ES SIGNIFICATIVO: de el depende la numeracion de los
    //      render-verts sueltos al recargar.
    if (m->looseEdges.size() >= 2) {
        int n = (int)(m->looseEdges.size() / 2);
        out += "\n# geometria SUELTA (aristas y vertices sin cara)\n";
        CabBlock(out, "EDGE", n, W3DM_EDGE); out += "\n";
        for (size_t i = 0; i + 1 < m->looseEdges.size(); i += 2) {
            int a = m->looseEdges[i], b = m->looseEdges[i + 1];
            out += ' '; W3dWriteInt(out, (a >= 0 && a < nV) ? rvToPoint[(size_t)a] : -1);
            out += ' '; W3dWriteInt(out, (b >= 0 && b < nV) ? rvToPoint[(size_t)b] : -1);
            out += "\n";
        }
        out += "END\n";
    }
    if (!m->looseVerts.empty()) {
        out += "\n"; CabBlock(out, "POINT", (int)m->looseVerts.size(), W3DM_POINT); out += "\n";
        for (size_t i = 0; i < m->looseVerts.size(); i++) {
            int a = m->looseVerts[i];
            out += ' '; W3dWriteInt(out, (a >= 0 && a < nV) ? rvToPoint[(size_t)a] : -1); out += "\n";
        }
        out += "END\n";
    }

    // ---- PARTS / PARTMAT: de MaterialGroup SOLO el nombre y el vinculo al
    //      material son DATOS; start/count los rehace RegroupMeshParts.
    if (!m->materialsGroup.empty()) {
        out += "\n# partes de malla (el material vive en la escena: se referencia POR NOMBRE)\n";
        CabBlock(out, "PARTS", (int)m->materialsGroup.size(), W3DM_GLOBAL); out += "\n";
        for (size_t i = 0; i < m->materialsGroup.size(); i++) {
            // ESCAPE: un mesh part llamado "END" cerraria el bloque (ver W3dmEscapePayload)
            out += ' ';
            out += W3dmEscapePayload(W3dmSaneName(m->materialsGroup[i].name, notices, "de un mesh part"));
            out += "\n";
        }
        out += "END\n";
        int n = 0;
        for (size_t i = 0; i < m->materialsGroup.size(); i++)
            if (m->materialsGroup[i].material && !m->materialsGroup[i].material->name.empty()) n++;
        if (n > 0) {
            out += "\n"; CabBlock(out, "PARTMAT", n, W3DM_GLOBAL); out += "\n";
            for (size_t i = 0; i < m->materialsGroup.size(); i++)
                if (m->materialsGroup[i].material && !m->materialsGroup[i].material->name.empty()) {
                    out += ' '; W3dWriteInt(out, (int)i);
                    out += ' '; out += W3dmSaneName(m->materialsGroup[i].material->name, notices, "de un material"); out += "\n";
                }
            out += "END\n";
        }
    }

    // ---- VG: vertex groups (pesos por CONTROL-POINT = por PUNTO). Hoy se
    //      guardan por la POSICION del punto porque el GLB re-splitea y los
    //      indices no sobreviven; aca los INDICES SON el dato.
    // control-point -> PRIMER render-vert que lo usa (se arma UNA vez, no por grupo)
    std::map<int, int> cpRep;
    W3dMeshCpRep(m, cpRep);
    for (size_t g = 0; g < m->vertexGroups.size(); g++) {
        // el pasaje control-point -> punto (y el aviso de los pesos que chocan) es
        // W3dMeshGroupByPoint, el mismo que usa el .w3db
        std::vector<int> pi;
        std::vector<float> pw;
        if (!W3dMeshGroupByPoint(m, P, cpRep, g, pi, pw, notices)) continue;
        const VertexGroup* vg = m->vertexGroups[g];
        out += "\n"; CabBlock(out, "VG", (int)pi.size(), W3DM_POINT);
        out += " name="; out += W3dmSaneName(vg->name, notices, "de un vertex group"); out += "\n";
        for (size_t k = 0; k < pi.size(); k++) {
            out += ' '; W3dWriteInt(out, pi[k]);
            out += ' '; W3dWriteFloat(out, pw[k]); out += "\n";
        }
        out += "END\n";
    }

    // ---- SHAPE: shape keys (formas de la malla: la cara que habla). Esparsas POR PUNTO, con el MISMO pasaje
    //      control-point -> punto que los vertex groups (cpRep + rvToPoint): "p dx dy dz" por linea. Los pesos NO
    //      se guardan aca (los anima la escena, canal AnimShape).
    for (size_t k = 0; k < m->shapeKeys.size(); k++) {
        const W3dShapeKey& sk = m->shapeKeys[k];
        std::vector<int> pi; std::vector<float> pd;
        W3dMeshShapeByPoint(m, P, cpRep, k, pi, pd);
        out += "\n"; CabBlock(out, "SHAPE", (int)pi.size(), W3DM_POINT);
        out += " name="; out += W3dmSaneName(sk.name, notices, "de una shape key"); out += "\n";
        for (size_t q = 0; q < pi.size(); q++) {
            out += ' '; W3dWriteInt(out, pi[q]);
            out += ' '; W3dWriteFloat(out, pd[q*3]);
            out += ' '; W3dWriteFloat(out, pd[q*3+1]);
            out += ' '; W3dWriteFloat(out, pd[q*3+2]); out += "\n";
        }
        out += "END\n";
    }

    // ---- UVG: UV groups. En memoria son por RENDER-VERT; en el archivo van
    //      POR ESQUINA y se colapsan al leer (lossless: todas las esquinas de un
    //      render-vert comparten el peso por construccion).
    for (size_t g = 0; g < m->uvGroups.size(); g++) {
        std::vector<int> ci; std::vector<float> cw;
        if (!W3dMeshUVGroupByCorner(m, g, ci, cw)) continue;
        const UVGroup* ug = m->uvGroups[g];
        out += "\n"; CabBlock(out, "UVG", (int)ci.size(), W3DM_CORNER);
        out += " name="; out += W3dmSaneName(ug->name, notices, "de un UV group"); out += "\n";
        for (size_t k = 0; k < ci.size(); k++) {
            out += ' '; W3dWriteInt(out, ci[k]);
            out += ' '; W3dWriteFloat(out, cw[k]); out += "\n";
        }
        out += "END\n";
    }

    // ---- RVMAP: CACHE, no requisito. Se escribe SOLO cuando la malla tiene
    //      vertex animations (que es donde el indice de render-vert es dato de
    //      verdad, porque los keyframes estan indexados asi). En el 99% de las
    //      mallas el archivo queda limpio y el lector rehace el merge.
    if (withAnim && nC > 0) {
        out += "\n# render-vert de cada esquina (ancla de las vertex anims)\n";
        CabBlock(out, "RVMAP", nC, W3DM_CORNER);
        out += " rverts="; W3dWriteInt(out, nV); out += "\n";
        int col = 0;
        for (size_t f = 0; f < m->faces3d.size(); f++)
            for (size_t c = 0; c < m->faces3d[f].idx.size(); c++) {
                if (col == 0) out += ' ';
                W3dWriteInt(out, m->faces3d[f].idx[c]);
                if (++col == 12) { out += "\n"; col = 0; } else out += ' ';
            }
        if (col != 0) out += "\n";
        out += "END\n";
    }

    // ---- BLOQUES DE UNA VERSION MAS NUEVA (preservacion) ----------------
    //  Regla que un lector viejo puede aplicar SIN ENTENDER NADA del bloque:
    //  dominio global -> se reescribe siempre; dominio indexado -> solo si la
    //  cantidad de ese dominio NO cambio. Si cambio se DESCARTA con aviso,
    //  porque re-escribir indices corridos seria PEOR que perderlos.
    // los bloques BINARIOS de una version mas nueva (vinieron de un .w3db) no tienen
    // representacion en texto: se pierden, pero NO en silencio.
    if (!m->w3dmForeign.blocksBin.empty() && notices) {
        std::string s2 = "no pude conservar ";
        W3dWriteInt(s2, (int)m->w3dmForeign.blocksBin.size());
        s2 += " bloque(s) binario(s) de una version mas nueva de Whisk3D (";
        for (size_t i = 0; i < m->w3dmForeign.blocksBin.size(); i++) {
            if (i) s2 += " ";
            s2 += m->w3dmForeign.blocksBin[i].name;
        }
        s2 += "): el formato de texto no los puede llevar; guarda en binario para conservarlos";
        notices->push_back(s2);
    }
    if (!m->w3dmForeign.blocks.empty()) {
        // cantidad de aristas UNICAS (pares de puntos sin repetir) de F + EDGE:
        // es el MISMO set que valida SHARP/SEAM, armado una sola vez mas arriba.
        const int nA = (int)realEdges.size();
        bool any = false;
        for (size_t i = 0; i < m->w3dmForeign.blocks.size(); i++) {
            const W3dForeignBlock& b = m->w3dmForeign.blocks[i];
            int now = -1, prev = -1;
            switch (b.domain) {
                case W3DM_POINT:   now = nP; prev = m->w3dmForeign.nPoints;   break;
                case W3DM_CORNER: now  = nC; prev = m->w3dmForeign.nCorners; break;
                case W3DM_FACE:    now = nF; prev = m->w3dmForeign.nFaces;    break;
                case W3DM_EDGE:  now   = nA; prev = m->w3dmForeign.nEdges;  break;
                default: break; // global: siempre
            }
            if (b.domain != W3DM_GLOBAL && now != prev) {
                if (notices) {
                    std::string s = "no pude conservar el bloque " + b.name + " (";
                    W3dWriteInt(s, prev); s += " -> "; W3dWriteInt(s, now); s += " ";
                    s += W3dmDomName(b.domain);
                    s += "): lo trajo una version mas nueva de Whisk3D y cambiaste la topologia";
                    notices->push_back(s);
                }
                continue;
            }
            if (!any) { out += "\n# bloques de una version mas nueva de Whisk3D\n"; any = true; }
            out += b.text;
        }
    }
    return true;
}

// ===========================================================================
//  quien descarto que (para el aviso del editor). Devuelve la lista de bloques
//  ajenos que NO sobrevivirian a un guardado con la topologia actual.
// ===========================================================================

// ===========================================================================
// ===========================================================================
//                                L E C T U R A
// ===========================================================================
// ===========================================================================

namespace {

struct LayerCorner {   // una capa UV o COL leida del archivo
    std::string name;
    bool perVertex;
    std::vector<float>         uv;
    std::vector<unsigned char> col;
    LayerCorner() : perVertex(false) {}
};
struct ReadGroup {
    std::string name;
    std::vector<int>   idx;
    std::vector<float> weight;
    std::vector<float> d;      // SHAPE: 3 por idx (dx, dy, dz)
};

// cabecera de bloque ya parseada
struct Cab {
    std::string name;
    int count;
    int domain;
    std::string label;   // valor de nombre=
    bool perVertex;      // clave por=vertice
    int  rvertsKey;      // clave rverts=
    Cab() : count(0), domain(W3DM_DOMMAL), perVertex(false), rvertsKey(-1) {}
};

bool ParseCab(const char* p, const char* end, Cab& out) {
    if (!W3dReadWord(p, end, out.name)) return false;
    if (!W3dReadInteger(p, end, out.count)) return false;
    std::string dom;
    if (!W3dReadWord(p, end, dom)) return false;
    out.domain = W3dmDomOf(dom);
    if (out.domain == W3DM_DOMMAL) return false;
    // extras: clave=valor. "name=" va ULTIMO y toma el RESTO de la linea.
    for (;;) {
        W3dSkipWhitespace(p, end);
        if (p >= end) break;
        if ((size_t)(end - p) >= 5 && memcmp(p, "name=", 5) == 0) {
            // TAL CUAL: el nombre arranca justo despues del '=' y llega hasta el
            // final de la linea, bordes incluidos (ver W3dLineName).
            out.label.assign(p + 5, (size_t)(end - (p + 5)));
            break;
        }
        std::string kv;
        if (!W3dReadWord(p, end, kv)) break;
        if (kv == "by=vertex") out.perVertex = true;
        else if (kv.size() > 5 && kv.compare(0, 7, "rverts=") == 0) {
            const char* q = kv.c_str() + 7;
            const char* qf = kv.c_str() + kv.size();
            int v = 0; if (W3dReadInteger(q, qf, v)) out.rvertsKey = v;
        }
        // una clave que no conocemos se IGNORA (es como se agregan campos nuevos)
    }
    if (out.count < 0) out.count = 0;
    return true;
}

} // namespace

bool W3dMeshReadText(const std::string& txt, Mesh* m, W3dMeshInfo* info) {
    return W3dMeshRead(txt.empty() ? "" : txt.data(), txt.size(), m, info);
}

bool W3dMeshRead(const char* data, size_t n, Mesh* m, W3dMeshInfo* info) {
    if (!m || !data) return false;
    W3dLines lin(data, n);
    const char* li; const char* lf;

    // ---- CABECERA: "W3DMESH <version del LEXICO>". Agregar bloques NUNCA la
    //      sube; un lector que ve una version mayor SE NIEGA y lo dice.
    if (!lin.Next(li, lf)) return false;
    {
        const char* p = li;
        std::string w;
        if (!W3dReadWord(p, lf, w) || w != "W3DMESH") return false;
        int show = 0;
        if (!W3dReadInteger(p, lf, show)) show = 0;
        if (show > 1) {
            W3dmNotice(info, 1, "este .w3dm es de una version del formato mas nueva; no lo puedo leer");
            return false;
        }
    }

    // ---- acumuladores ---------------------------------------------------
    std::string name;
    int declPoints = -1, declCorners = -1, declFaces = -1, declRverts = -1;
    int smooth = 1, uvActive = -1, activeCol = -1, vgActive = -1, activeUvg = -1;
    int primType = -1; float primS = 2.0f, primS2 = 0.0f, primD = 0.0f; int primV = 8, primV2 = 8;
    bool hasSmooth = false, hasPrim = false;

    std::vector<float> pts;
    std::vector<int>   facesSides, facesIdx;
    std::vector<int>   fpartF, fpartV, fshadeF, fshadeV;
    std::vector<signed char> nrm;
    std::vector<LayerCorner>  layersUV, layersCol;
    std::vector<float> uvrest;
    std::vector<int>   sharp, seam, looseEdge, loosePoint;
    std::vector<std::string> parts;
    std::vector<int> partmatI; std::vector<std::string> partmatN;
    std::vector<ReadGroup> vgs, uvgs, shapes;
    std::vector<int> rvmap; int rvmapRverts = -1;
    std::vector<W3dForeignBlock> foreign;
    std::vector<std::string> requiresValue;
    // los DOS bloques que el propio archivo declara obligatorios ("requiere V F").
    // Que aparezcan es lo unico que distingue "una malla vacia" de "el archivo se
    // corto" (ver el chequeo de abajo).
    bool showedV = false, showedF = false;

    // ---- recorrido por lineas ------------------------------------------
    while (lin.Next(li, lf)) {
        if (li >= lf) continue;                 // linea vacia
        char c0 = *li;
        if (c0 == '#') continue;                // comentario
        if (c0 == ' ' || c0 == '\t') continue;  // dato sin bloque abierto: se ignora

        if (c0 >= 'a' && c0 <= 'z') {           // ---------------- DIRECTIVA
            const char* p = li;
            std::string k;
            W3dReadWord(p, lf, k);
            if      (k == "name")   name = W3dLineName(p, lf);
            else if (k == "points")   W3dReadInteger(p, lf, declPoints);
            else if (k == "corners") W3dReadInteger(p, lf, declCorners);
            else if (k == "faces")    W3dReadInteger(p, lf, declFaces);
            else if (k == "rverts")   W3dReadInteger(p, lf, declRverts);
            else if (k == "smooth")   { W3dReadInteger(p, lf, smooth); hasSmooth = true; }
            else if (k == "activeuv")  W3dReadInteger(p, lf, uvActive);
            else if (k == "activecol") W3dReadInteger(p, lf, activeCol);
            else if (k == "activevg")  W3dReadInteger(p, lf, vgActive);
            else if (k == "activeuvg") W3dReadInteger(p, lf, activeUvg);
            else if (k == "requires") {
                std::string b;
                while (W3dReadWord(p, lf, b)) requiresValue.push_back(b);
            } else if (k == "prim") {
                std::string t;
                if (W3dReadWord(p, lf, t)) {
                    primType = -1;
                    for (int i = 0; i < kPrimN; i++) if (t == kPrim[i]) { primType = i; break; }
                    if (primType < 0) {
                        const char* q = t.c_str(); const char* qf = q + t.size();
                        if (!W3dReadInteger(q, qf, primType)) primType = -1;
                    }
                    W3dReadFloat(p, lf, primS); W3dReadFloat(p, lf, primS2); W3dReadFloat(p, lf, primD);
                    W3dReadInteger(p, lf, primV); W3dReadInteger(p, lf, primV2);
                    hasPrim = (primType >= 0);
                }
            }
            // una directiva desconocida se IGNORA a proposito (es como se
            // agregan campos nuevos sin romper a los lectores viejos)
            continue;
        }

        if (!(c0 >= 'A' && c0 <= 'Z')) continue; // basura: se ignora

        // ---------------- BLOQUE ----------------
        Cab cab;
        const int lineCab = lin.Num();
        if (!ParseCab(li, lf, cab)) {
            W3dmNotice(info, lineCab, "cabecera de bloque mal formada; salteo el bloque");
            // igual hay que consumir hasta END para no descolocar el resto
            while (lin.Next(li, lf)) if (W3dmIsEnd(li, lf)) break;
            continue;
        }
        const bool known = W3dMeshBlockKnown(cab.name);

        // --- bloque DESCONOCIDO: se guarda VERBATIM (cabecera .. END) ---
        if (!known) {
            W3dForeignBlock settings;
            settings.name = cab.name;
            settings.domain = cab.domain;
            settings.count = cab.count;
            settings.text.assign(li, (size_t)(lf - li));
            settings.text += "\n";
            bool closed = false;
            while (lin.Next(li, lf)) {
                settings.text.append(li, (size_t)(lf - li));
                settings.text += "\n";
                if (W3dmIsEnd(li, lf)) { closed = true; break; }
            }
            if (!closed) { if (info) info->truncated = true; W3dmNotice(info, lineCab, "bloque sin END (archivo truncado)"); }
            foreign.push_back(settings);
            continue;
        }

        // --- bloque CONOCIDO: se lee el payload linea por linea ---
        if      (cab.name == "V") showedV = true;
        else if (cab.name == "F") showedF = true;
        const size_t remains = lin.Remaining();
        int cnt = cab.count;
        // reserva ACOTADA por el tamano real que queda del archivo
        if      (cab.name == "V")      { pts.reserve((size_t)W3dmBound(cnt, remains, 3) * 3); }
        else if (cab.name == "F")      { facesSides.reserve((size_t)W3dmBound(cnt, remains, 4)); }
        else if (cab.name == "NRM")    { nrm.reserve((size_t)W3dmBound(cnt, remains, 3) * 3); }

        LayerCorner layer; layer.name = cab.label; layer.perVertex = cab.perVertex;
        ReadGroup group; group.name = cab.label;
        if (cab.name == "UV")  layer.uv.reserve((size_t)W3dmBound(cnt, remains, 2) * 2);
        if (cab.name == "COL") layer.col.reserve((size_t)W3dmBound(cnt, remains, 4) * 4);
        if (cab.name == "RVMAP") { rvmap.reserve((size_t)W3dmBound(cnt, remains, 1)); rvmapRverts = cab.rvertsKey; }

        int read    = 0;
        bool closed = false;
        while (lin.Next(li, lf)) {
            if (W3dmIsEnd(li, lf)) { closed = true; break; }
            if (li >= lf) continue;
            if (*li == '#') continue;
            if (!(*li == ' ' || *li == '\t')) {
                // otra cabecera/directiva ANTES del END: el bloque estaba mal cerrado.
                W3dmNotice(info, lin.Num(), "bloque sin END; lo cierro aca");
                closed = true;
                break;
            }
            const char* p = li;
            if (cab.name == "V") {
                float x = 0, y = 0, z = 0;
                W3dReadFloat(p, lf, x); W3dReadFloat(p, lf, y); W3dReadFloat(p, lf, z);
                pts.push_back(x); pts.push_back(y); pts.push_back(z);
            } else if (cab.name == "F") {
                int sides = 0;
                if (!W3dReadInteger(p, lf, sides)) continue;
                if (sides < 3 || sides > 100000) { W3dmNotice(info, lin.Num(), "cara con cantidad de lados invalida; la descarto"); continue; }
                std::vector<int> ring;
                // ACOTADO CONTRA LO QUE QUEDA DEL ARCHIVO, igual que todas las demas
                // reservas: "100000" por linea son 400 KB, y un archivo malo con
                // 200.000 lineas asi pedia 400 KB CADA UNA. En Symbian eso es un
                // bad_alloc sobre un heap fragmentado (y aca no hay try/catch).
                ring.reserve((size_t)W3dmBound(sides, (size_t)(lf - li), 1));
                bool ok = true;
                for (int i = 0; i < sides; i++) {
                    int v = 0;
                    if (!W3dReadInteger(p, lf, v)) { ok = false; break; }
                    ring.push_back(v);
                }
                if (!ok) { W3dmNotice(info, lin.Num(), "cara con menos indices que lados; la descarto"); continue; }
                facesSides.push_back(sides);
                for (int i = 0; i < sides; i++) facesIdx.push_back(ring[(size_t)i]);
            } else if (cab.name == "FPART" || cab.name == "FSHADE") {
                int a = 0, b = 0;
                if (!W3dReadInteger(p, lf, a) || !W3dReadInteger(p, lf, b)) continue;
                if (cab.name == "FPART") { fpartF.push_back(a); fpartV.push_back(b); }
                else                       { fshadeF.push_back(a); fshadeV.push_back(b); }
            } else if (cab.name == "NRM") {
                int x = 0, y = 0, z = 0;
                W3dReadInteger(p, lf, x); W3dReadInteger(p, lf, y); W3dReadInteger(p, lf, z);
                if (x < -128) x = -128; if (x > 127) x = 127;
                if (y < -128) y = -128; if (y > 127) y = 127;
                if (z < -128) z = -128; if (z > 127) z = 127;
                nrm.push_back((signed char)x); nrm.push_back((signed char)y); nrm.push_back((signed char)z);
            } else if (cab.name == "UV") {
                float u = 0, v = 0;
                W3dReadFloat(p, lf, u); W3dReadFloat(p, lf, v);
                layer.uv.push_back(u); layer.uv.push_back(v);
            } else if (cab.name == "UVREST") {
                float u = 0, v = 0;
                W3dReadFloat(p, lf, u); W3dReadFloat(p, lf, v);
                uvrest.push_back(u); uvrest.push_back(v);
            } else if (cab.name == "COL") {
                int r = 255, g = 255, b = 255, a = 255;
                W3dReadInteger(p, lf, r); W3dReadInteger(p, lf, g); W3dReadInteger(p, lf, b); W3dReadInteger(p, lf, a);
                if (r < 0) r = 0; if (r > 255) r = 255;
                if (g < 0) g = 0; if (g > 255) g = 255;
                if (b < 0) b = 0; if (b > 255) b = 255;
                if (a < 0) a = 0; if (a > 255) a = 255;
                layer.col.push_back((unsigned char)r); layer.col.push_back((unsigned char)g);
                layer.col.push_back((unsigned char)b); layer.col.push_back((unsigned char)a);
            } else if (cab.name == "SHARP" || cab.name == "SEAM" || cab.name == "EDGE") {
                int a = 0, b = 0;
                if (!W3dReadInteger(p, lf, a) || !W3dReadInteger(p, lf, b)) continue;
                std::vector<int>& dst = (cab.name == "SHARP") ? sharp : (cab.name == "SEAM" ? seam : looseEdge);
                dst.push_back(a); dst.push_back(b);
            } else if (cab.name == "POINT") {
                int a = 0;
                if (!W3dReadInteger(p, lf, a)) continue;
                loosePoint.push_back(a);
            } else if (cab.name == "PARTS") {
                parts.push_back(W3dmUnescapePayload(W3dLineName(p, lf)));
            } else if (cab.name == "PARTMAT") {
                int a = 0;
                if (!W3dReadInteger(p, lf, a)) continue;
                partmatI.push_back(a); partmatN.push_back(W3dLineName(p, lf));
            } else if (cab.name == "VG" || cab.name == "UVG") {
                int a = 0; float w = 0;
                if (!W3dReadInteger(p, lf, a)) continue;
                W3dReadFloat(p, lf, w);
                group.idx.push_back(a); group.weight.push_back(w);
            } else if (cab.name == "SHAPE") {
                int a = 0; float dx = 0, dy = 0, dz = 0;
                if (!W3dReadInteger(p, lf, a)) continue;
                W3dReadFloat(p, lf, dx); W3dReadFloat(p, lf, dy); W3dReadFloat(p, lf, dz);
                group.idx.push_back(a); group.d.push_back(dx); group.d.push_back(dy); group.d.push_back(dz);
            } else if (cab.name == "RVMAP") {
                int a = 0;
                while (W3dReadInteger(p, lf, a)) rvmap.push_back(a);
            }
            read++;
        }
        if (!closed) { if (info) info->truncated = true; W3dmNotice(info, lineCab, "bloque sin END (archivo truncado): uso lo que hay"); }
        // TOLERANCIA para el que edita a mano: la cantidad es para RESERVAR, no
        // un contrato. Si sobran o faltan lineas se usa lo que hay y se avisa.
        if (cab.name != "RVMAP" && cnt != read) {
            std::string s = "bloque "; s += cab.name; s += ": declaraba ";
            W3dWriteInt(s, cnt); s += " y encontre "; W3dWriteInt(s, read);
            W3dmNotice(info, lineCab, s);
        }
        if (cab.name == "UV")  layersUV.push_back(layer);
        if (cab.name == "COL") layersCol.push_back(layer);
        if (cab.name == "VG")  vgs.push_back(group);
        if (cab.name == "UVG") uvgs.push_back(group);
        if (cab.name == "SHAPE") shapes.push_back(group);
    }

    // ---- "requiere": si nombra un bloque que no conocemos, el archivo se abre
    //      pero NO se puede pisar (degradar en silencio seria peor).
    for (size_t i = 0; i < requiresValue.size(); i++)
        if (!W3dMeshBlockKnown(requiresValue[i])) {
            if (info) info->readOnly = true;
            W3dmNotice(info, 1, "el archivo REQUIERE el bloque '" + requiresValue[i] +
                               "', que esta version no entiende: lo abro en SOLO LECTURA");
        }

    // =======================================================================
    //  ESTAN LOS BLOQUES OBLIGATORIOS?
    //
    //  Antes esto no se miraba y era el peor de los caminos silenciosos: un
    //  archivo cortado JUSTO EN UN END (un zip copiado a medias, un disco que se
    //  lleno a mitad de la copia) no deja ningun bloque abierto, asi que
    //  'truncado' no se prendia, no salia UN SOLO aviso... y el volcado de mas
    //  abajo igual borraba vertex/normals/uv/vertexColor y devolvia true con
    //  vertexSize=0. O sea: la malla del usuario se VACIABA y el editor lo
    //  reportaba como una lectura exitosa.
    //
    //  Se corta ACA, ANTES de tocar la malla: el que llama se queda con la malla
    //  que tenia y con el aviso. Una malla legitimamente vacia igual escribe sus
    //  bloques "V 0 punto"/"F 0 cara", asi que no hay falso positivo.
    // =======================================================================
    if (!showedV || !showedF) {
        std::string s = "el archivo no trae el bloque ";
        if (!showedV && !showedF) s += "V ni el bloque F";
        else if (!showedV)     s += "V";
        else                s += "F";
        s += " (esta cortado o no es una malla): NO toco la malla que ya estaba";
        W3dmNotice(info, lin.Num(), s);
        if (info) {
            info->truncated = true;
            if (info->noticesOfMore > 0) {
                std::string t = "(y "; W3dWriteInt(t, info->noticesOfMore);
                t += " aviso(s) mas que no muestro)";
                info->notices.push_back(t);
            }
        }
        return false;
    }

    // =======================================================================
    //  RECONSTRUCCION
    // =======================================================================
    const int nP = (int)(pts.size() / 3);
    const int nF = (int)facesSides.size();
    int nC = 0;
    for (int f = 0; f < nF; f++) nC += facesSides[(size_t)f];

    // validacion de indices: fuera de rango = se descarta la CARA y se avisa.
    //
    //  LAS CAPAS SE RECORTAN, NO SE TIRAN. Antes UN SOLO indice mal tipeado en UNA
    //  linea F borraba TODAS las capas por esquina de la malla (layersUV.clear() +
    //  layersCol.clear() + nrm + uvrest): en un formato que se vende como editable a
    //  mano, un typo no puede costar todo el trabajo de texturizado. Y ademas el
    //  costo era invisible: sin capas UV el escritor emitia despues una "UVMap"
    //  DERIVADA llena de ceros, que PARECE dato y no lo es.
    //  Aca ya sabemos EXACTAMENTE que esquinas se fueron (las de las caras
    //  descartadas), asi que se arma la mascara de esquinas SOBREVIVIENTES y se
    //  recorta cada capa con ella. Lo unico que se pierde es lo que estaba en las
    //  caras rotas, que es lo que de verdad se perdio.
    {
        size_t off = 0;
        std::vector<int> sides2, idx2;
        sides2.reserve((size_t)nF);
        idx2.reserve(facesIdx.size());
        int bad = 0;
        for (int f = 0; f < nF; f++) {
            int L = facesSides[(size_t)f];
            bool ok = true;
            for (int i = 0; i < L; i++) {
                int v = facesIdx[off + (size_t)i];
                if (v < 0 || v >= nP) { ok = false; break; }
            }
            if (ok) {
                sides2.push_back(L);
                for (int i = 0; i < L; i++) idx2.push_back(facesIdx[off + (size_t)i]);
            } else bad++;
            off += (size_t)L;
        }
        if (bad > 0) {
            // la mascara de esquinas sobrevivientes se arma RECIEN ACA (segunda
            // pasada): en el camino normal -sin caras rotas- no se reserva ni un
            // byte de mas, que en el N95 es la diferencia entre cargar y fragmentar.
            std::vector<int> survC;
            survC.reserve((size_t)nC);
            size_t o2 = 0; int L0 = 0;
            for (int f = 0; f < nF; f++) {
                int L = facesSides[(size_t)f];
                bool ok = true;
                for (int i = 0; i < L; i++) {
                    int v = facesIdx[o2 + (size_t)i];
                    if (v < 0 || v >= nP) { ok = false; break; }
                }
                if (ok) for (int i = 0; i < L; i++) survC.push_back(L0 + i);
                o2 += (size_t)L;
                L0 += L;
            }
            std::string s = "descarte "; W3dWriteInt(s, bad);
            s += " cara(s) con indices de punto fuera de rango (recorto las capas por esquina a las caras que quedaron)";
            W3dmNotice(info, 1, s);
            const int nCviejo = nC;
            for (size_t l = 0; l < layersUV.size(); l++) {
                if ((int)layersUV[l].uv.size() != nCviejo * 2) continue;  // ya no cuadraba: la agarra el chequeo de abajo
                std::vector<float> nu; nu.reserve(survC.size() * 2);
                for (size_t i = 0; i < survC.size(); i++) {
                    nu.push_back(layersUV[l].uv[(size_t)survC[i] * 2]);
                    nu.push_back(layersUV[l].uv[(size_t)survC[i] * 2 + 1]);
                }
                layersUV[l].uv.swap(nu);
            }
            for (size_t l = 0; l < layersCol.size(); l++) {
                if ((int)layersCol[l].col.size() != nCviejo * 4) continue;
                std::vector<unsigned char> nc; nc.reserve(survC.size() * 4);
                for (size_t i = 0; i < survC.size(); i++)
                    for (int q = 0; q < 4; q++) nc.push_back(layersCol[l].col[(size_t)survC[i] * 4 + (size_t)q]);
                layersCol[l].col.swap(nc);
            }
            if ((int)nrm.size() == nCviejo * 3) {
                std::vector<signed char> nn; nn.reserve(survC.size() * 3);
                for (size_t i = 0; i < survC.size(); i++)
                    for (int q = 0; q < 3; q++) nn.push_back(nrm[(size_t)survC[i] * 3 + (size_t)q]);
                nrm.swap(nn);
            }
            if ((int)uvrest.size() == nCviejo * 2) {
                std::vector<float> nr; nr.reserve(survC.size() * 2);
                for (size_t i = 0; i < survC.size(); i++) {
                    nr.push_back(uvrest[(size_t)survC[i] * 2]);
                    nr.push_back(uvrest[(size_t)survC[i] * 2 + 1]);
                }
                uvrest.swap(nr);
            }
            // los UV groups tambien indexan ESQUINAS: se remapean al indice nuevo y
            // los pesos que caian en una cara descartada se van con ella.
            if (!uvgs.empty()) {
                std::vector<int> oldToNew((size_t)(nCviejo > 0 ? nCviejo : 1), -1);
                for (size_t i = 0; i < survC.size(); i++) oldToNew[(size_t)survC[i]] = (int)i;
                for (size_t g = 0; g < uvgs.size(); g++) {
                    std::vector<int> ni; std::vector<float> np;
                    for (size_t k = 0; k < uvgs[g].idx.size() && k < uvgs[g].weight.size(); k++) {
                        int L = uvgs[g].idx[k];
                        if (L < 0 || L >= nCviejo || oldToNew[(size_t)L] < 0) continue;
                        ni.push_back(oldToNew[(size_t)L]); np.push_back(uvgs[g].weight[k]);
                    }
                    uvgs[g].idx.swap(ni); uvgs[g].weight.swap(np);
                }
            }
            facesSides.swap(sides2); facesIdx.swap(idx2);
            nC = 0; for (size_t f = 0; f < facesSides.size(); f++) nC += facesSides[f];
        }
    }
    const int nFok = (int)facesSides.size();

    // capas por esquina que no midan lo que tienen que medir: se descartan
    for (size_t i = 0; i < layersUV.size(); ) {
        if ((int)layersUV[i].uv.size() != nC * 2) {
            W3dmNotice(info, 1, "la capa UV '" + layersUV[i].name + "' no cubre todas las esquinas: la descarto");
            layersUV.erase(layersUV.begin() + (long)i);
        } else i++;
    }
    for (size_t i = 0; i < layersCol.size(); ) {
        if ((int)layersCol[i].col.size() != nC * 4) {
            W3dmNotice(info, 1, "la capa de color '" + layersCol[i].name + "' no cubre todas las esquinas: la descarto");
            layersCol.erase(layersCol.begin() + (long)i);
        } else i++;
    }
    if (!nrm.empty() && (int)nrm.size() != nC * 3) { W3dmNotice(info, 1, "NRM no cubre todas las esquinas: recalculo las normales"); nrm.clear(); }
    if (!uvrest.empty() && (int)uvrest.size() != nC * 2) { W3dmNotice(info, 1, "UVREST no cubre todas las esquinas: lo descarto"); uvrest.clear(); }

    if (uvActive >= (int)layersUV.size())  uvActive  = layersUV.empty()  ? -1 : 0;
    if (activeCol >= (int)layersCol.size()) activeCol = layersCol.empty() ? -1 : 0;
    if (uvActive < -1)  uvActive = -1;
    if (activeCol < -1) activeCol = -1;

    const LayerCorner* uvAct  = (uvActive  >= 0) ? &layersUV[(size_t)uvActive]   : 0;
    const LayerCorner* colAct = (activeCol >= 0 && !layersCol[(size_t)activeCol].perVertex)
                               ? &layersCol[(size_t)activeCol] : 0;

    // ---- MERGE DE RENDER ------------------------------------------------
    //  MISMA clave de 40 bytes y MISMO orden de primera aparicion que
    //  GenerateRender (libs/Whisk3DCore/edit/MeshEdit.cpp): pos(12) + uv active(8) +
    //  normal por corner(3) + color active(4). Por eso el array de render se
    //  reproduce BIT A BIT sin haberlo guardado.
    std::vector<float>         vp;
    std::vector<signed char>   vn;
    std::vector<float>         vu;
    std::vector<unsigned char> vc;
    std::vector<int> pointToRender((size_t)(nP > 0 ? nP : 1), -1);
    // render-vert -> punto. Espejo del de arriba, pero COMPLETO: pointToRender se
    // queda con el PRIMER render-vert de cada punto, y una posicion puede tener
    // varios render-verts (una costura de UV o de color parte el vertice: son
    // unicos por posicion+color+UV y no se pueden fusionar sin romper el
    // sombreado ni las UV). Los pesos de los vertex groups se guardan POR
    // POSICION, asi que al cargar hay que repartirlos a TODOS.
    std::vector<int> renderToPoint;
    std::vector<MeshFace> faces;
    faces.reserve((size_t)nFok);
    vp.reserve((size_t)nC * 3); vn.reserve((size_t)nC * 3);
    vu.reserve((size_t)nC * 2); vc.reserve((size_t)nC * 4);

    // RVMAP valido? Si lo esta, MANDA el (ancla los keyframes de las vertex
    // anims). Si no cierra se ignora, se rehace el merge y se avisa: un RVMAP
    // mentiroso degrada a lento, nunca a roto.
    bool useRvmap = false;
    int  rvN = 0;
    if ((int)rvmap.size() == nC && nC > 0) {
        int mx = -1; bool ok = true;
        for (int i = 0; i < nC && ok; i++) { if (rvmap[(size_t)i] < 0) ok = false; else if (rvmap[(size_t)i] > mx) mx = rvmap[(size_t)i]; }
        if (ok && mx >= 0 && mx < nC + 8) {
            std::vector<char> seen((size_t)mx + 1, 0);
            for (int i = 0; i < nC; i++) seen[(size_t)rvmap[(size_t)i]] = 1;
            for (int i = 0; i <= mx && ok; i++) if (!seen[(size_t)i]) ok = false; // tiene que cubrir 0..mx
            if (ok) { useRvmap = true; rvN = mx + 1; }
        }
        if (!useRvmap) W3dmNotice(info, 1, "RVMAP inconsistente: lo ignoro y rehago el merge de render");
    } else if (!rvmap.empty()) {
        W3dmNotice(info, 1, "RVMAP no cubre todas las esquinas: lo ignoro");
    }

    if (useRvmap) {
        vp.assign((size_t)rvN * 3, 0.0f);
        vn.assign((size_t)rvN * 3, 0);
        vu.assign((size_t)rvN * 2, 0.0f);
        vc.assign((size_t)rvN * 4, 255);
        std::vector<char> placed((size_t)rvN, 0);
        size_t off = 0; int L = 0;
        for (int f = 0; f < nFok; f++) {
            MeshFace mf;
            int sides = facesSides[(size_t)f];
            mf.idx.reserve((size_t)sides);
            for (int i = 0; i < sides; i++, L++) {
                int pi = facesIdx[off + (size_t)i];
                int gi = rvmap[(size_t)L];
                if (!placed[(size_t)gi]) {
                    placed[(size_t)gi] = 1;
                    vp[(size_t)gi * 3] = pts[(size_t)pi * 3]; vp[(size_t)gi * 3 + 1] = pts[(size_t)pi * 3 + 1]; vp[(size_t)gi * 3 + 2] = pts[(size_t)pi * 3 + 2];
                    if (!nrm.empty()) { vn[(size_t)gi * 3] = nrm[(size_t)L * 3]; vn[(size_t)gi * 3 + 1] = nrm[(size_t)L * 3 + 1]; vn[(size_t)gi * 3 + 2] = nrm[(size_t)L * 3 + 2]; }
                    else { vn[(size_t)gi * 3] = 0; vn[(size_t)gi * 3 + 1] = 127; vn[(size_t)gi * 3 + 2] = 0; }
                    if (uvAct) { vu[(size_t)gi * 2] = uvAct->uv[(size_t)L * 2]; vu[(size_t)gi * 2 + 1] = uvAct->uv[(size_t)L * 2 + 1]; }
                    if (colAct) for (int k = 0; k < 4; k++) vc[(size_t)gi * 4 + (size_t)k] = colAct->col[(size_t)L * 4 + (size_t)k];
                }
                if (pointToRender[(size_t)pi] < 0) pointToRender[(size_t)pi] = gi;
                if ((int)renderToPoint.size() <= gi) renderToPoint.resize((size_t)gi + 1, -1);
                renderToPoint[(size_t)gi] = pi;
                mf.idx.push_back(gi);
            }
            off += (size_t)sides;
            faces.push_back(mf);
        }
    } else {
        std::map<std::string, int> map;
        size_t off = 0; int L = 0;
        for (int f = 0; f < nFok; f++) {
            MeshFace mf;
            int sides = facesSides[(size_t)f];
            mf.idx.reserve((size_t)sides);
            for (int i = 0; i < sides; i++, L++) {
                int pi = facesIdx[off + (size_t)i];
                float px = pts[(size_t)pi * 3], py = pts[(size_t)pi * 3 + 1], pz = pts[(size_t)pi * 3 + 2];
                signed char nbx = 0, nby = 127, nbz = 0;
                if (!nrm.empty()) { nbx = nrm[(size_t)L * 3]; nby = nrm[(size_t)L * 3 + 1]; nbz = nrm[(size_t)L * 3 + 2]; }
                float u0 = uvAct ? uvAct->uv[(size_t)L * 2] : 0.0f;
                float v0 = uvAct ? uvAct->uv[(size_t)L * 2 + 1] : 0.0f;
                unsigned char r = 255, g = 255, b = 255, a = 255;
                if (colAct) { r = colAct->col[(size_t)L * 4]; g = colAct->col[(size_t)L * 4 + 1]; b = colAct->col[(size_t)L * 4 + 2]; a = colAct->col[(size_t)L * 4 + 3]; }
                // el PUNTO es parte de la clave: dos puntos distintos en la misma posicion (con otros
                // pesos) nunca se funden en un render-vert; las esquinas del MISMO punto con la misma
                // UV / normal / color si (eso es lo que une las caras)
                char buf[44]; int q = 0;
                memcpy(buf + q, &pi, 4); q += 4;
                memcpy(buf + q, &px, 4); q += 4; memcpy(buf + q, &py, 4); q += 4; memcpy(buf + q, &pz, 4); q += 4;
                memcpy(buf + q, &u0, 4); q += 4; memcpy(buf + q, &v0, 4); q += 4;
                buf[q++] = (char)nbx; buf[q++] = (char)nby; buf[q++] = (char)nbz;
                buf[q++] = (char)r; buf[q++] = (char)g; buf[q++] = (char)b; buf[q++] = (char)a;
                std::string key(buf, (size_t)q);
                std::map<std::string, int>::iterator it = map.find(key);
                int gi;
                if (it != map.end()) gi = it->second;
                else {
                    gi = (int)(vp.size() / 3);
                    vp.push_back(px); vp.push_back(py); vp.push_back(pz);
                    vn.push_back(nbx); vn.push_back(nby); vn.push_back(nbz);
                    vu.push_back(u0); vu.push_back(v0);
                    vc.push_back(r); vc.push_back(g); vc.push_back(b); vc.push_back(a);
                    map[key] = gi;
                }
                if (pointToRender[(size_t)pi] < 0) pointToRender[(size_t)pi] = gi;
                if ((int)renderToPoint.size() <= gi) renderToPoint.resize((size_t)gi + 1, -1);
                renderToPoint[(size_t)gi] = pi;
                mf.idx.push_back(gi);
            }
            off += (size_t)sides;
            faces.push_back(mf);
        }
    }

    // ---- GEOMETRIA SUELTA: se anexa DESPUES de las caras y en el MISMO orden
    //      que usa GenerateRender (primero EDGE, despues POINT), asi la
    //      numeracion de los render-verts sueltos coincide.
    std::vector<int> nLoose, nLooseV;
    for (size_t i = 0; i + 1 < looseEdge.size(); i += 2) {
        int ab[2] = { looseEdge[i], looseEdge[i + 1] }, nn[2] = { -1, -1 };
        for (int s = 0; s < 2; s++) {
            int p = ab[s];
            if (p < 0 || p >= nP) { nn[s] = -1; continue; }
            if (pointToRender[(size_t)p] >= 0) { nn[s] = pointToRender[(size_t)p]; continue; }
            int gi = (int)(vp.size() / 3);
            vp.push_back(pts[(size_t)p * 3]); vp.push_back(pts[(size_t)p * 3 + 1]); vp.push_back(pts[(size_t)p * 3 + 2]);
            vn.push_back(0); vn.push_back(127); vn.push_back(0);
            vu.push_back(0.0f); vu.push_back(0.0f);
            vc.push_back(255); vc.push_back(255); vc.push_back(255); vc.push_back(255);
            pointToRender[(size_t)p] = gi; nn[s] = gi;
            if ((int)renderToPoint.size() <= gi) renderToPoint.resize((size_t)gi + 1, -1);
            renderToPoint[(size_t)gi] = p;
        }
        if (nn[0] >= 0 && nn[1] >= 0) { nLoose.push_back(nn[0]); nLoose.push_back(nn[1]); }
        else W3dmNotice(info, 1, "arista suelta con un punto fuera de rango: la descarto");
    }
    for (size_t i = 0; i < loosePoint.size(); i++) {
        int p = loosePoint[i];
        if (p < 0 || p >= nP) { W3dmNotice(info, 1, "vertice suelto fuera de rango: lo descarto"); continue; }
        // ya lo usa una cara/arista (o OTRO vertice suelto): no es un vertice aparte.
        // El escritor de Whisk3D nunca emite esto (a cada vertice suelto le da su propio
        // punto justamente para no soldarlos), asi que si aparece es de un archivo
        // editado a mano o de otro programa. Se fusiona, pero NO en silencio.
        if (pointToRender[(size_t)p] >= 0) {
            W3dmNotice(info, 1, "un vertice suelto cae en un punto que ya esta en uso: lo fusiono con el que ya estaba");
            continue;
        }
        int gi = (int)(vp.size() / 3);
        vp.push_back(pts[(size_t)p * 3]); vp.push_back(pts[(size_t)p * 3 + 1]); vp.push_back(pts[(size_t)p * 3 + 2]);
        vn.push_back(0); vn.push_back(127); vn.push_back(0);
        vu.push_back(0.0f); vu.push_back(0.0f);
        vc.push_back(255); vc.push_back(255); vc.push_back(255); vc.push_back(255);
        pointToRender[(size_t)p] = gi; nLooseV.push_back(gi);
        if ((int)renderToPoint.size() <= gi) renderToPoint.resize((size_t)gi + 1, -1);
        renderToPoint[(size_t)gi] = p;
    }

    const int nRV = (int)(vp.size() / 3);
    if (info) { info->rverts = declRverts; info->realRverts = nRV; }
    if (declRverts >= 0 && declRverts != nRV) {
        std::string s = "los vertices de render no dieron lo declarado (";
        W3dWriteInt(s, declRverts); s += " contra "; W3dWriteInt(s, nRV);
        s += "): las vertex anims van a necesitar el remapeo por posicion";
        W3dmNotice(info, 1, s);
    }

    // =======================================================================
    //  VOLCADO A LA MALLA
    // =======================================================================
    delete[] m->vertex;      m->vertex = 0;
    delete[] m->normals;     m->normals = 0;
    delete[] m->uv;          m->uv = 0;
    delete[] m->vertexColor; m->vertexColor = 0;
    m->vertexSize = nRV;
    // ESTADO DE POSE: el .w3dm guarda SIEMPRE la malla EN REPOSO (el escritor la devuelve al
    // cuadro base con W3dRestVertexAnim), asi que lo que se acaba de leer ES el modelo. El
    // estado viaja con las posiciones tambien por esta puerta: se escribe explicito y no se
    // hereda lo que hubiera en la malla destino. Ver Mesh::posedByAnim.
    m->posedByAnim = false;
    if (nRV > 0) {
        m->vertex      = new GLfloat[(size_t)nRV * 3];
        m->normals     = new GLbyte[(size_t)nRV * 3];
        m->uv          = new GLfloat[(size_t)nRV * 2];
        m->vertexColor = new GLubyte[(size_t)nRV * 4];
        for (int i = 0; i < nRV * 3; i++) { m->vertex[i] = vp[(size_t)i]; m->normals[i] = (GLbyte)vn[(size_t)i]; }
        for (int i = 0; i < nRV * 2; i++) m->uv[i] = vu[(size_t)i];
        for (int i = 0; i < nRV * 4; i++) m->vertexColor[i] = (GLubyte)vc[(size_t)i];
    }

    if (!name.empty()) m->name = name;
    if (hasSmooth) m->meshSmooth = (smooth != 0);
    if (hasPrim) {
        m->meshType = primType; m->meshSize = primS; m->meshSize2 = primS2;
        m->meshDepth = primD;   m->meshVerts = primV; m->meshVerts2 = primV2;
    } else m->meshType = -1;

    // caras + su mesh part y su shading por cara
    for (size_t i = 0; i < fpartF.size(); i++) {
        int f = fpartF[i];
        if (f >= 0 && f < (int)faces.size()) faces[(size_t)f].mat = fpartV[i];
        else W3dmNotice(info, 1, "FPART apunta a una cara que no existe: lo descarto");
    }
    for (size_t i = 0; i < fshadeF.size(); i++) {
        int f = fshadeF[i];
        if (f >= 0 && f < (int)faces.size()) faces[(size_t)f].smooth = (fshadeV[i] != 0) ? 1 : 0;
        else W3dmNotice(info, 1, "FSHADE apunta a una cara que no existe: lo descarto");
    }
    m->faces3d.swap(faces);
    m->looseEdges.swap(nLoose);
    m->looseVerts.swap(nLooseV);

    // normales por esquina (dato AUTORITATIVO del usuario)
    m->cornerNormal.clear();
    if (!nrm.empty()) {
        m->cornerNormal.resize(nrm.size());
        for (size_t i = 0; i < nrm.size(); i++) m->cornerNormal[i] = (GLbyte)nrm[i];
    }

    // capas
    W3dmFreeLayers(m);
    for (size_t l = 0; l < layersUV.size(); l++) {
        UVMap* u = new UVMap(layersUV[l].name);
        u->uv.assign(layersUV[l].uv.begin(), layersUV[l].uv.end());
        m->uvMaps.push_back(u);
    }
    m->uvActiveMap = uvActive;
    for (size_t l = 0; l < layersCol.size(); l++) {
        ColorLayer* c = new ColorLayer(layersCol[l].name);
        c->perVertex  = layersCol[l].perVertex;
        c->color.assign(layersCol[l].col.begin(), layersCol[l].col.end());
        m->colorLayers.push_back(c);
    }
    m->activeColor = activeCol;

    // UVREST: de ESQUINA a RENDER-VERT (colapso lossless)
    m->uv2dRest.clear();
    if (!uvrest.empty() && nRV > 0) {
        m->uv2dRest.assign((size_t)nRV * 2, 0.0f);
        int L = 0;
        for (size_t f = 0; f < m->faces3d.size(); f++)
            for (size_t c = 0; c < m->faces3d[f].idx.size(); c++, L++) {
                int gv = m->faces3d[f].idx[c];
                if (gv >= 0 && gv < nRV) { m->uv2dRest[(size_t)gv * 2] = uvrest[(size_t)L * 2]; m->uv2dRest[(size_t)gv * 2 + 1] = uvrest[(size_t)L * 2 + 1]; }
            }
    }

    // ---- VERTEX GROUPS: el indice guardado es el PUNTO --------------------
    //  El peso viaja POR POSICION, y una posicion puede tener VARIOS render-verts:
    //  una costura de UV o de color parte el vertice (son unicos por
    //  posicion + color + UV y no se pueden fusionar sin romper el sombreado ni
    //  las UV). Antes el peso se le daba SOLO al representante -- pointToRender[p],
    //  el primer render-vert de esa posicion -- y los demas quedaban sin peso: en
    //  una malla grande con costuras se perdian cientos de pesos y el grupo
    //  quedaba incompleto (una animacion por grupos partia la malla en dos por la
    //  costura). Ahora el peso de una posicion va a TODOS sus render-verts.
    std::vector<std::vector<int> > rvsOfPoint;
    if ((!vgs.empty() || !shapes.empty()) && nP > 0) {
        rvsOfPoint.resize((size_t)nP);
        for (int gi = 0; gi < nRV && gi < (int)renderToPoint.size(); gi++) {
            int p = renderToPoint[(size_t)gi];
            if (p >= 0 && p < nP) rvsOfPoint[(size_t)p].push_back(gi);
        }
    }
    for (size_t g = 0; g < vgs.size(); g++) {
        VertexGroup* vg = new VertexGroup(vgs[g].name);
        int distributed = 0;
        for (size_t k = 0; k < vgs[g].idx.size(); k++) {
            int p = vgs[g].idx[k];
            if (p < 0 || p >= nP || pointToRender[(size_t)p] < 0) { W3dmNotice(info, 1, "peso de vertex group con punto fuera de rango: lo descarto"); continue; }
            const float w = vgs[g].weight[k];
            const std::vector<int>* rvs = rvsOfPoint.empty() ? NULL : &rvsOfPoint[(size_t)p];
            if (rvs && !rvs->empty()) {
                for (size_t i = 0; i < rvs->size(); i++) {
                    vg->verts.push_back((*rvs)[i]);
                    vg->weights.push_back(w);
                }
                if (rvs->size() > 1) distributed += (int)rvs->size() - 1;
            } else {
                vg->verts.push_back(pointToRender[(size_t)p]);
                vg->weights.push_back(w);
            }
        }
        (void)distributed;   // el reparto es la conducta correcta: no se avisa
        m->vertexGroups.push_back(vg);
    }
    // SHAPE KEYS: de PUNTO a TODOS sus render-verts (el mismo espacio que los vertex groups de arriba, que es el
    // que usa el skinning por control-point)
    for (size_t g = 0; g < shapes.size(); g++) {
        W3dShapeKey sk; sk.name = shapes[g].name;
        for (size_t k = 0; k < shapes[g].idx.size() && k * 3 + 2 < shapes[g].d.size(); k++) {
            int p = shapes[g].idx[k];
            if (p < 0 || p >= nP || pointToRender[(size_t)p] < 0) continue;
            const std::vector<int>* rvs = rvsOfPoint.empty() ? NULL : &rvsOfPoint[(size_t)p];
            if (rvs && !rvs->empty()) {
                for (size_t i = 0; i < rvs->size(); i++) {
                    sk.idx.push_back((*rvs)[i]);
                    sk.d.push_back(shapes[g].d[k*3]); sk.d.push_back(shapes[g].d[k*3+1]); sk.d.push_back(shapes[g].d[k*3+2]);
                }
            } else {
                sk.idx.push_back(pointToRender[(size_t)p]);
                sk.d.push_back(shapes[g].d[k*3]); sk.d.push_back(shapes[g].d[k*3+1]); sk.d.push_back(shapes[g].d[k*3+2]);
            }
        }
        m->shapeKeys.push_back(sk);
    }
    m->shapeWeights.assign(m->shapeKeys.size(), 0.0f);
    // -1 ("ninguno activo") ES UN VALOR VALIDO y hay que respetarlo: normalizarlo a 0 hacia que
    // guardar -> abrir -> guardar diera bytes distintos por un dato que nadie cambio.
    m->activeGroup = (vgActive >= -1 && vgActive < (int)m->vertexGroups.size()) ? vgActive : (m->vertexGroups.empty() ? -1 : 0);

    // esquina -> render-vert, plano (se arma UNA vez: sin esto la busqueda seria
    // cuadratica y en una malla grande el N95 se queda pensando)
    std::vector<int> cornerToRender;
    cornerToRender.reserve((size_t)nC);
    for (size_t f = 0; f < m->faces3d.size(); f++)
        for (size_t c = 0; c < m->faces3d[f].idx.size(); c++)
            cornerToRender.push_back(m->faces3d[f].idx[c]);

    // UV groups: de ESQUINA a RENDER-VERT
    for (size_t g = 0; g < uvgs.size(); g++) {
        UVGroup* ug = new UVGroup(uvgs[g].name);
        std::vector<float> w((size_t)(nRV > 0 ? nRV : 1), 0.0f);
        for (size_t k = 0; k < uvgs[g].idx.size(); k++) {
            int L = uvgs[g].idx[k];
            if (L < 0 || L >= (int)cornerToRender.size()) { W3dmNotice(info, 1, "peso de UV group con esquina fuera de rango: lo descarto"); continue; }
            int gv = cornerToRender[(size_t)L];
            if (gv >= 0 && gv < nRV) w[(size_t)gv] = uvgs[g].weight[k];
        }
        for (int i = 0; i < nRV; i++) if (w[(size_t)i] > 0.0f) { ug->verts.push_back(i); ug->weights.push_back(w[(size_t)i]); }
        m->uvGroups.push_back(ug);
    }
    m->uvActiveGroup = (activeUvg >= -1 && activeUvg < (int)m->uvGroups.size()) ? activeUvg : (m->uvGroups.empty() ? -1 : 0);

    // mesh parts (el material lo resuelve POR NOMBRE el que llama)
    m->materialsGroup.clear();
    if (info) info->materials.clear();
    for (size_t i = 0; i < parts.size(); i++) {
        MaterialGroup mg;
        mg.name = parts[i];
        m->materialsGroup.push_back(mg);
        if (info) info->materials.push_back(std::string());
    }
    if (m->materialsGroup.empty()) { m->materialsGroup.push_back(MaterialGroup()); if (info) info->materials.push_back(std::string()); }
    for (size_t i = 0; i < partmatI.size(); i++) {
        int p = partmatI[i];
        if (p >= 0 && p < (int)m->materialsGroup.size()) { if (info) info->materials[(size_t)p] = partmatN[i]; }
        else W3dmNotice(info, 1, "PARTMAT apunta a un mesh part que no existe: lo descarto");
    }
    // una cara que apunte a un mesh part inexistente cae al 0 (nunca se dibuja fuera de rango)
    for (size_t f = 0; f < m->faces3d.size(); f++)
        if (m->faces3d[f].mat < 0 || m->faces3d[f].mat >= (int)m->materialsGroup.size()) m->faces3d[f].mat = 0;

    // SHARP / SEAM: par de PUNTOS -> clave de POSICION (la misma que
    // Mesh::SharpEdgeKey), rearmada desde las posiciones ya leidas.
    m->sharpEdges.clear();
    m->seamEdges.clear();
    for (int pass = 0; pass < 2; pass++) {
        const std::vector<int>& src = pass ? seam : sharp;
        std::set<std::string>& dst = pass ? m->seamEdges : m->sharpEdges;
        for (size_t i = 0; i + 1 < src.size(); i += 2) {
            int a = src[i], b = src[i + 1];
            if (a < 0 || a >= nP || b < 0 || b >= nP || a == b) { W3dmNotice(info, 1, "arista marcada con un punto fuera de rango: la descarto"); continue; }
            dst.insert(W3dmEdgeKey(&pts[(size_t)a * 3], &pts[(size_t)b * 3]));
        }
    }

    // ---- SELLO + BLOQUES AJENOS ----------------------------------------
    m->w3dmForeign.Clear();
    m->w3dmForeign.blocks = foreign;
    // la directiva "requiere" VIAJA CON LA MALLA: el escritor la re-emite tal cual.
    // Sin esto se auto-borraba en el primer guardado y el archivo se quedaba sin la
    // valvula que protege a los bloques que esta version no entiende.
    m->w3dmForeign.requiresValue = requiresValue;
    m->w3dmForeign.truncated     = (info ? info->truncated : false);
    m->w3dmForeign.nPoints       = nP;
    m->w3dmForeign.nCorners      = nC;
    m->w3dmForeign.nFaces        = (int)m->faces3d.size();
    {
        std::set<std::pair<int, int> > ar;
        size_t off = 0;
        for (size_t f = 0; f < facesSides.size(); f++) {
            int L = facesSides[f];
            for (int i = 0; i < L; i++) {
                int a = facesIdx[off + (size_t)i], b = facesIdx[off + (size_t)((i + 1) % L)];
                if (a != b) ar.insert(a < b ? std::make_pair(a, b) : std::make_pair(b, a));
            }
            off += (size_t)L;
        }
        for (size_t i = 0; i + 1 < looseEdge.size(); i += 2) {
            int a = looseEdge[i], b = looseEdge[i + 1];
            if (a >= 0 && a < nP && b >= 0 && b < nP && a != b)
                ar.insert(a < b ? std::make_pair(a, b) : std::make_pair(b, a));
        }
        m->w3dmForeign.nEdges = (int)ar.size();
    }

    // LOS PUNTOS DEL ARCHIVO SON LA IDENTIDAD DE LOS VERTICES: render-vert -> el primer render-vert de SU
    // punto, en el mapa del skinning (vertCtrlPoint) y en el agrupamiento del editor (posRep). Antes el mapa
    // se rehacia POR POSICION al cargar y dos puntos distintos parados en el mismo lugar (cada uno con su
    // hueso) terminaban usando los pesos del primero: la malla se estiraba al animar. El que carga tiene que
    // conservar este posRep (ComputeBorders(..., regroupPosRep = false)).
    m->vertCtrlPoint.assign((size_t)nRV, 0);
    m->posRep.assign((size_t)nRV, 0);
    for (int gi = 0; gi < nRV; gi++) {
        int p = (gi < (int)renderToPoint.size()) ? renderToPoint[(size_t)gi] : -1;
        int r = (p >= 0 && p < nP && pointToRender[(size_t)p] >= 0) ? pointToRender[(size_t)p] : gi;
        if (r > gi) r = gi;          // el representante es el MENOR indice (contrato de posRep)
        m->vertCtrlPoint[(size_t)gi] = r;
        m->posRep[(size_t)gi] = r;
    }
    W3dMeshJoinByPosition(m);   // (los archivos de antes: la costura y los polos, ver W3dmPosQuant)
    m->uvSelVert.clear();
    // el techo de avisos no puede esconder que hay mas (ver W3dMeshInfo::avisos)
    if (info && info->noticesOfMore > 0) {
        std::string t = "(y "; W3dWriteInt(t, info->noticesOfMore);
        t += " aviso(s) mas que no muestro)";
        info->notices.push_back(t);
    }
    return true;
}
