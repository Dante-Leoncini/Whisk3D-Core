#include "W3dMalla.h"
#include "W3dTexto.h"
#include "W3dMallaBin.h"   // W3dMallaBinBloqueConocido: el freno "requiere" del binario
#include "objects/Mesh.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <cstring>

// libera las capas de la malla SIN pasar por Mesh::LiberarCapas: ese metodo vive
// en el EDITOR (main/edit/MeshEdit.cpp) y el juego compilado lo tiene STUBEADO en
// vacio, asi que llamarlo desde aca leakearia en el runtime. Este archivo entra
// en las dos builds y tiene que valerse por si mismo.
static void W3dmLiberarCapas(Mesh* m) {
    for (size_t i = 0; i < m->uvMaps.size(); i++)       delete m->uvMaps[i];
    for (size_t i = 0; i < m->colorLayers.size(); i++)  delete m->colorLayers[i];
    for (size_t i = 0; i < m->vertexGroups.size(); i++) delete m->vertexGroups[i];
    for (size_t i = 0; i < m->uvGroups.size(); i++)     delete m->uvGroups[i];
    m->uvMaps.clear();       m->uvMapActivo = -1;
    m->colorLayers.clear();  m->colorActivo = -1;
    m->vertexGroups.clear(); m->grupoActivo = -1;
    m->uvGroups.clear();     m->uvGrupoActivo = -1;
}

// ===========================================================================
//  VOCABULARIO CERRADO DE DOMINIOS
//  Cerrarlo en cinco no es una apuesta: en dos anios de Mesh.h todo lo que se
//  agrego cayo en uno de estos cinco lugares. Y cerrarlo es lo que garantiza
//  que dentro de cinco anios un lector VIEJO sepa QUE FORMA tiene un bloque que
//  no conoce, que es de lo que depende la preservacion.
// ===========================================================================
enum { W3DM_PUNTO = 0, W3DM_ESQUINA = 1, W3DM_CARA = 2, W3DM_ARISTA = 3, W3DM_GLOBAL = 4, W3DM_DOMMAL = -1 };

static const char* W3dmDomNombre(int d) {
    switch (d) {
        case W3DM_PUNTO:   return "punto";
        case W3DM_ESQUINA: return "esquina";
        case W3DM_CARA:    return "cara";
        case W3DM_ARISTA:  return "arista";
        default:           return "global";
    }
}
static int W3dmDomDe(const std::string& s) {
    if (s == "punto")   return W3DM_PUNTO;
    if (s == "esquina") return W3DM_ESQUINA;
    if (s == "cara")    return W3DM_CARA;
    if (s == "arista")  return W3DM_ARISTA;
    if (s == "global")  return W3DM_GLOBAL;
    return W3DM_DOMMAL;
}

// bloques que ESTA version entiende (registro: Whisk3D/formato/bloques.tsv)
static const char* kBloques[] = {
    "V", "F", "FPART", "FSHADE", "NRM", "UV", "COL", "UVREST",
    "SHARP", "SEAM", "EDGE", "POINT", "PARTS", "PARTMAT", "VG", "UVG", "RVMAP", "SHAPE", 0
};
bool W3dMallaBloqueConocido(const std::string& nombre) {
    for (int i = 0; kBloques[i]; i++) if (nombre == kBloques[i]) return true;
    return false;
}

// nombres de primitiva (orden de MeshType). El numero tambien se acepta al leer.
static const char* kPrim[] = { "cubo", "esfera", "ico", "plano", "vertice", "circulo", "cono", "cilindro" };
static const int   kPrimN = 8;

// ---------------------------------------------------------------------------
//  clave de POSICION: los 12 bytes CRUDOS de los 3 floats. Es la MISMA que usa
//  Mesh::SharpEdgeKey (main/edit/MeshEdit.cpp) y por eso el escritor de floats
//  tiene que round-trippear EXACTO: si un decimal vuelve como otro float, la
//  costura no matchea y se pierde EN SILENCIO.
// ---------------------------------------------------------------------------
static std::string W3dmPosKey(const float* p) {
    char b[12]; memcpy(b, p, 12); return std::string(b, 12);
}
static std::string W3dmAristaKey(const float* a, const float* b) {
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
static int W3dmAcotar(int declarada, size_t bytesQueQuedan, int camposPorElemento) {
    if (declarada <= 0) return 0;
    if (camposPorElemento < 1) camposPorElemento = 1;
    size_t cota = bytesQueQuedan / (size_t)(2 * camposPorElemento);
    if ((size_t)declarada > cota) return (int)cota;
    return declarada;
}

// ---------------------------------------------------------------------------
//  AVISOS ACOTADOS. Un .w3dm roto de 2 MB puede tener 200.000 lineas malas: la
//  lista de avisos era LO UNICO del lector sin techo y crecia decenas de MB por
//  un archivo que ya sabemos que esta mal. Se guardan los primeros
//  kW3dmMaxAvisos y el resto se CUENTA (info->avisosDeMas), asi el techo no
//  esconde nada: el renglon final lo dice.
// ---------------------------------------------------------------------------
static void W3dmAviso(W3dMallaInfo* info, int linea, const std::string& txt) {
    if (!info) return;
    if ((int)info->avisos.size() >= (int)kW3dmMaxAvisos) { info->avisosDeMas++; return; }
    std::string s = "linea ";
    W3dEscribirInt(s, linea);
    s += ": ";
    s += txt;
    info->avisos.push_back(s);
}

// ---------------------------------------------------------------------------
//  "END" TOLERANTE. Comparar los 3 bytes pelados hacia que un espacio INVISIBLE
//  despues del END ("END ") no matcheara: el bloque seguia abierto y se tragaba
//  el resto del archivo (incluido el bloque V) sin un solo aviso. Para un formato
//  que se vende como editable a mano eso es una trampa mortal, asi que se
//  recortan blancos de los dos bordes y se acepta en cualquier caja.
// ---------------------------------------------------------------------------
static bool W3dmEsEnd(const char* li, const char* lf) {
    while (li < lf && (*li == ' ' || *li == '\t')) li++;
    while (lf > li && (*(lf - 1) == ' ' || *(lf - 1) == '\t')) lf--;
    if (lf - li != 3) return false;
    return (li[0] == 'E' || li[0] == 'e') && (li[1] == 'N' || li[1] == 'n') &&
           (li[2] == 'D' || li[2] == 'd');
}

// ---------------------------------------------------------------------------
//  ESCAPE DEL PAYLOAD DE 'PARTS' (ver el comentario grande de W3dMalla.h).
//  PARTS es el UNICO bloque cuyo dato es un nombre pelado, asi que un mesh part
//  llamado "END" emitia una linea que el END TOLERANTE recortaba y aceptaba: el
//  bloque se cerraba de mas, el END verdadero se leia como cabecera mal formada y
//  se comia el bloque PARTMAT entero (la malla perdia las partes Y sus
//  materiales). Se escapa con UN '\' adelante y el lector saca UNO: lossless,
//  ancho 1 igual que siempre, y no hace falta tocar el END tolerante (que es lo
//  que hace que el formato se banque editarse a mano).
// ---------------------------------------------------------------------------
static bool W3dmNombreChocaConEnd(const std::string& n) {
    const char* a = n.c_str();
    return W3dmEsEnd(a, a + n.size());
}
static std::string W3dmEscaparPayload(const std::string& n) {
    if (W3dmNombreChocaConEnd(n) || (!n.empty() && n[0] == '\\')) return "\\" + n;
    return n;
}
static std::string W3dmDesescaparPayload(const std::string& n) {
    if (!n.empty() && n[0] == '\\') return n.substr(1);
    return n;
}

// ---------------------------------------------------------------------------
//  NOMBRES AL ESCRIBIR. El unico caracter que el formato NO puede representar en
//  un nombre es el SALTO DE LINEA: partiria el archivo en dos y la mitad de abajo
//  se leeria como bloques sueltos. El escritor NO puede emitirlo nunca (aunque el
//  nombre venga de un .json editado a mano o de un importador), asi que se
//  reemplaza por un espacio y SE AVISA. Los espacios de los bordes SI se
//  conservan: el lector los respeta (W3dNombreLinea).
// ---------------------------------------------------------------------------
static std::string W3dmNombreSano(const std::string& n, std::vector<std::string>* avisos,
                                  const char* que) {
    bool sucio = false;
    for (size_t i = 0; i < n.size(); i++)
        if (n[i] == '\n' || n[i] == '\r') { sucio = true; break; }
    if (!sucio) return n;
    std::string s = n;
    for (size_t i = 0; i < s.size(); i++) if (s[i] == '\n' || s[i] == '\r') s[i] = ' ';
    if (avisos) {
        std::string a = "el nombre ";
        a += que;
        a += " tenia un salto de linea (partiria el archivo en dos): lo guardo como '";
        a += s; a += "'";
        avisos->push_back(a);
    }
    return s;
}
std::string W3dMallaNombreSano(const std::string& n, std::vector<std::string>* avisos, const char* que) {
    return W3dmNombreSano(n, avisos, que);
}

// ===========================================================================
// ===========================================================================
//                              E S C R I T U R A
// ===========================================================================
// ===========================================================================

static void CabBloque(std::string& s, const char* nombre, int cantidad, int dominio) {
    s += nombre; s += ' ';
    W3dEscribirInt(s, cantidad); s += ' ';
    s += W3dmDomNombre(dominio);
}

// ---------------------------------------------------------------------------
//  EL FRENO DE MANO (spec seccion 17). Si el archivo dijo "requiere <X>" y X no
//  esta en kBloques, esta version NO puede interpretar bien la malla: guardar
//  encima la degradaria EN SILENCIO, que es justo lo que esa directiva existe
//  para impedir. Devuelve el nombre del primer bloque que falta ("" = se puede
//  guardar). Se mira la lista GUARDADA, no un flag: asi sigue dando la respuesta
//  correcta cuando una version futura aprenda el bloque.
// ---------------------------------------------------------------------------
std::string W3dMallaBloqueQueFalta(const Mesh* m) {
    if (!m) return std::string();
    for (size_t i = 0; i < m->w3dmAjenos.requiere.size(); i++)
        if (!W3dMallaBloqueConocido(m->w3dmAjenos.requiere[i])) return m->w3dmAjenos.requiere[i];
    // el MISMO freno para la malla binaria: su bloque REQ nombro un fourcc que esta version
    // no entiende (ver W3dAjenos::requiereBin y io/W3dMallaBin.h)
    for (size_t i = 0; i < m->w3dmAjenos.requiereBin.size(); i++)
        if (!W3dMallaBinBloqueConocido(m->w3dmAjenos.requiereBin[i])) return m->w3dmAjenos.requiereBin[i];
    return std::string();
}

// ===========================================================================
//  PIEZAS COMPARTIDAS CON EL .w3db (ver el bloque grande del final de W3dMalla.h).
//  Son pedazos de W3dMallaEscribir sacados afuera TAL CUAL, en el mismo orden de
//  avisos: el escritor de texto los llama en el mismo lugar donde estaban.
// ===========================================================================

void W3dMallaCalcularPuntos(const Mesh* m, W3dMallaPuntos& out) {
    out.rvAPunto.clear(); out.pts.clear(); out.porPos.clear();
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
    //  el usuario los ve como dos y GenerarRender NO los une. Si el archivo los
    //  colapsara en un punto, "agregar vertice / Shift+D / Esc" seguido de guardar
    //  perderia uno SIN AVISO, y eso no es "lo que ya hacia el editor": es topologia
    //  que cambia el formato. Entonces los render-verts SUELTOS (looseVerts y los
    //  extremos de looseEdges que no usa ninguna cara) se llevan un punto PROPIO;
    //  render-verts sueltos que SI son el mismo (dos aristas sueltas encadenadas)
    //  comparten punto, porque comparten indice de render.
    std::vector<int>&  rvAPunto = out.rvAPunto;
    rvAPunto.assign((size_t)(nV > 0 ? nV : 1), -1);
    std::vector<char>  rvDeCara((size_t)(nV > 0 ? nV : 1), 0);
    std::vector<char>  rvSuelto((size_t)(nV > 0 ? nV : 1), 0);
    for (size_t f = 0; f < m->faces3d.size(); f++)
        for (size_t c = 0; c < m->faces3d[f].idx.size(); c++) {
            int gv = m->faces3d[f].idx[c];
            if (gv >= 0 && gv < nV) rvDeCara[(size_t)gv] = 1;
        }
    for (size_t i = 0; i < m->looseEdges.size(); i++) {
        int gv = m->looseEdges[i];
        if (gv >= 0 && gv < nV && !rvDeCara[(size_t)gv]) rvSuelto[(size_t)gv] = 1;
    }
    for (size_t i = 0; i < m->looseVerts.size(); i++) {
        int gv = m->looseVerts[i];
        if (gv >= 0 && gv < nV && !rvDeCara[(size_t)gv]) rvSuelto[(size_t)gv] = 1;
    }
    // ---- LOS DATOS DEL PUNTO tambien lo identifican. Dos render-verts en la MISMA
    //      posicion son el mismo punto solo si ademas tienen los MISMOS datos de punto
    //      (pesos de cada vertex group, deltas de cada shape key). Una costura de UV o de
    //      color los tiene iguales (es el mismo vertice partido) y se une como siempre;
    //      pero dos vertices DISTINTOS que solo coinciden en el lugar (el borde de una
    //      puerta pegado a la carroceria, cada uno de su hueso) NO son el mismo: soldarlos
    //      al guardar mezclaba sus pesos y la malla se estiraba al animar. Guardar nunca
    //      suelda lo que la malla no tiene soldado.
    std::vector<std::string> firmaRv((size_t)(nV > 0 ? nV : 1));
    if (nV > 0 && (!m->vertexGroups.empty() || !m->shapeKeys.empty())) {
        const bool conCp = ((int)m->vertCtrlPoint.size() == nV);
        std::map<int, std::string> firmaCp;   // control-point -> sus datos (bytes)
        for (size_t g = 0; g < m->vertexGroups.size(); g++) {
            const VertexGroup* vg = m->vertexGroups[g];
            if (!vg) continue;
            for (size_t k = 0; k < vg->verts.size() && k < vg->pesos.size(); k++) {
                if (vg->pesos[k] == 0.0f) continue;          // peso 0 = no estar en el grupo
                char b[8]; int gi = (int)g; float w = vg->pesos[k];
                memcpy(b, &gi, 4); memcpy(b + 4, &w, 4);
                firmaCp[vg->verts[k]].append(b, 8);
            }
        }
        for (size_t g = 0; g < m->shapeKeys.size(); g++) {
            const W3dShapeKey& sk = m->shapeKeys[g];
            for (size_t j = 0; j < sk.idx.size() && j * 3 + 2 < sk.d.size(); j++) {
                char b[17]; int gi = (int)g; b[0] = 'S';
                memcpy(b + 1, &gi, 4); memcpy(b + 5, &sk.d[j * 3], 12);
                firmaCp[sk.idx[j]].append(b, 17);
            }
        }
        for (int i = 0; i < nV; i++) {
            int cp = conCp ? m->vertCtrlPoint[(size_t)i] : i;
            std::map<int, std::string>::iterator it = firmaCp.find(cp);
            if (it != firmaCp.end()) firmaRv[(size_t)i] = it->second;
        }
    }
    std::vector<float>& pts = out.pts;
    // posicion -> TODOS los puntos parados ahi, de menor a mayor (sharp/seam se indexan por
    // posicion). Casi siempre es uno solo; son varios cuando la malla tiene vertices distintos
    // en el mismo lugar (los de arriba, con otros pesos) o un punto suelto encima de una cara.
    std::map<std::string, std::vector<int> >& mapaP = out.porPos;
    std::map<std::string, int> mapaPD;     // posicion + datos del punto -> punto
    pts.reserve((size_t)nV * 3);
    for (int i = 0; i < nV; i++) {
        std::string k = W3dmPosKey(&m->vertex[i * 3]);
        std::string kd = k + firmaRv[(size_t)i];
        if (!rvSuelto[(size_t)i]) {
            std::map<std::string, int>::iterator it = mapaPD.find(kd);
            if (it != mapaPD.end()) { rvAPunto[(size_t)i] = it->second; continue; }
        }
        int pi = (int)(pts.size() / 3);
        pts.push_back(m->vertex[i * 3]);
        pts.push_back(m->vertex[i * 3 + 1]);
        pts.push_back(m->vertex[i * 3 + 2]);
        // TODO punto nuevo entra al mapa de POSICION (tambien el suelto): sharp/seam se
        // indexan por posicion y tienen que poder resolverla contra el punto de SU arista.
        mapaP[k].push_back(pi);
        if (mapaPD.find(kd) == mapaPD.end()) mapaPD[kd] = pi;
        rvAPunto[(size_t)i] = pi;
    }
    out.nP = (int)(pts.size() / 3);
}

void W3dMallaElegirCapas(const Mesh* m, int nC, W3dMallaCapasSalen& out, std::vector<std::string>* avisos) {
    out.uv.clear(); out.col.clear();
    out.uvActiva = -1; out.colActiva = -1; out.uvDerivada = false; out.colDerivada = false;
    if (!m) return;
    const int nV = (m->vertex ? m->vertexSize : 0);
    // CAPAS LAZY (ver el comentario grande del bloque NRM): si la malla todavia no
    // materializo sus capas, el archivo lleva igual la version derivada del render y la
    // capa 0 es la activa.
    out.uvDerivada = (m->uvMaps.empty() && m->uv && nC > 0 && nV > 0);
    if (m->colorLayers.empty() && m->vertexColor && nC > 0 && nV > 0)
        for (int i = 0; i < nV * 4 && !out.colDerivada; i++) if (m->vertexColor[i] != 255) out.colDerivada = true;

    // ---- QUE CAPAS SALEN, DECIDIDO ANTES DE ESCRIBIR NADA ------------------
    //  Una capa que no mide lo que la topologia dice NO se puede escribir (el
    //  bloque es POR ESQUINA), pero saltearla en silencio rompia DOS promesas:
    //  la del header ("si algo no sobrevive, se dice") y, peor, la del indice:
    //  "uvactiva"/"colactiva" se escriben ARRIBA con el indice de la lista
    //  COMPLETA, asi que saltear una capa ANTERIOR a la activa dejaba el indice
    //  apuntando a OTRA capa y la malla volvia con otro uv/color en el render.
    //  Por eso el indice que se escribe es el de la lista REALMENTE escrita.
    std::vector<size_t>& uvSalen = out.uv;
    std::vector<size_t>& colSalen = out.col;
    for (size_t l = 0; l < m->uvMaps.size(); l++) {
        const UVMap* u = m->uvMaps[l];
        if (!u || (int)u->uv.size() != nC * 2) {
            if (avisos) {
                std::string s = "no pude guardar la capa UV '";
                s += (u ? u->nombre : std::string("?"));
                s += "': tiene "; W3dEscribirInt(s, u ? (int)(u->uv.size() / 2) : 0);
                s += " esquinas y la malla tiene "; W3dEscribirInt(s, nC);
                avisos->push_back(s);
            }
            continue;
        }
        uvSalen.push_back(l);
    }
    for (size_t l = 0; l < m->colorLayers.size(); l++) {
        const ColorLayer* c = m->colorLayers[l];
        if (!c || (int)c->color.size() != nC * 4) {
            if (avisos) {
                std::string s = "no pude guardar la capa de color '";
                s += (c ? c->nombre : std::string("?"));
                s += "': tiene "; W3dEscribirInt(s, c ? (int)(c->color.size() / 4) : 0);
                s += " esquinas y la malla tiene "; W3dEscribirInt(s, nC);
                avisos->push_back(s);
            }
            continue;
        }
        colSalen.push_back(l);
    }
    int uvActivaSale = -1, colActivaSale = -1;
    for (size_t k = 0; k < uvSalen.size(); k++)  if ((int)uvSalen[k]  == m->uvMapActivo) uvActivaSale  = (int)k;
    for (size_t k = 0; k < colSalen.size(); k++) if ((int)colSalen[k] == m->colorActivo) colActivaSale = (int)k;
    // un indice FUERA DE RANGO (estado inconsistente en memoria, no una capa
    // descartada) cae a la capa 0: es exactamente lo que hacia el lector al
    // clampearlo, y ese comportamiento no se toca.
    if (m->uvMapActivo  >= (int)m->uvMaps.size()      && !uvSalen.empty())  uvActivaSale  = 0;
    if (m->colorActivo  >= (int)m->colorLayers.size() && !colSalen.empty()) colActivaSale = 0;
    if (avisos && m->uvMapActivo >= 0 && m->uvMapActivo < (int)m->uvMaps.size() && uvActivaSale < 0)
        avisos->push_back("la capa UV ACTIVA no se pudo guardar: el archivo queda sin capa UV activa");
    if (avisos && m->colorActivo >= 0 && m->colorActivo < (int)m->colorLayers.size() && colActivaSale < 0)
        avisos->push_back("la capa de color ACTIVA no se pudo guardar: el archivo queda sin capa de color activa");
    out.uvActiva = uvActivaSale;
    out.colActiva = colActivaSale;
}

void W3dMallaAristasReales(const Mesh* m, const W3dMallaPuntos& P, std::set<std::pair<int, int> >& out) {
    out.clear();
    if (!m) return;
    const int nV = P.nV;
    const std::vector<int>& rvAPunto = P.rvAPunto;
    for (size_t f = 0; f < m->faces3d.size(); f++) {
        const MeshFace& F = m->faces3d[f];
        for (size_t c = 0; c < F.idx.size(); c++) {
            int ga = F.idx[c], gb = F.idx[(c + 1) % F.idx.size()];
            if (ga < 0 || ga >= nV || gb < 0 || gb >= nV) continue;
            int a = rvAPunto[(size_t)ga], b = rvAPunto[(size_t)gb];
            if (a == b) continue;
            out.insert(a < b ? std::make_pair(a, b) : std::make_pair(b, a));
        }
    }
    for (size_t i = 0; i + 1 < m->looseEdges.size(); i += 2) {
        int ga = m->looseEdges[i], gb = m->looseEdges[i + 1];
        if (ga < 0 || ga >= nV || gb < 0 || gb >= nV) continue;
        int a = rvAPunto[(size_t)ga], b = rvAPunto[(size_t)gb];
        if (a == b) continue;
        out.insert(a < b ? std::make_pair(a, b) : std::make_pair(b, a));
    }
}

void W3dMallaMarcasValidas(const Mesh* m, const W3dMallaPuntos& P, const std::set<std::pair<int, int> >& aristasReales,
                           bool seam, std::vector<std::pair<int, int> >& pares, std::vector<std::string>* avisos) {
    pares.clear();
    if (!m) return;
    const std::set<std::string>& src = seam ? m->seamEdges : m->sharpEdges;
    if (src.empty()) return;
    const int nV = P.nV;
    // INVARIANTE DE POSE (el tercero de la clase; el mismo que respeta la poda,
    // ver Mesh::PodarMarcasSinArista y Mesh::PosicionesReposo): las claves de
    // sharp/seam viven en el espacio de REPOSO. Con la malla POSADA por una vertex
    // anim, 'mapaP' se armo con las posiciones DE LA POSE y NINGUNA clave resuelve
    // -> el escritor las daba TODAS por perdidas y el archivo salia sin UNA SOLA
    // marca. Ese es, literal, el sintoma que reporto el dueno: marcar, mover el
    // playhead, guardar. Aca el mapa de busqueda se rearma con las posiciones del
    // KEYFRAME BASE; rvAPunto (render-vert -> punto) NO depende de la pose, asi que
    // los indices que se escriben son exactamente los mismos.
    // (El guardado del proyecto ademas deja la malla en reposo antes de llamar
    //  -W3dReposoVertexAnim-, que es lo que evita ademas hornear la pose como
    //  geometria; esto cierra el camino directo al escritor.)
    std::map<std::string, std::vector<int> > mapaReposo;
    if (m->PosadaPorVertexAnim()) {
        const GLfloat* R = m->PosicionesReposo();
        if (R && R != m->vertex)
            for (int i = 0; i < nV; i++) {
                const int p = P.rvAPunto[(size_t)i];
                if (p < 0) continue;
                std::vector<int>& lista = mapaReposo[W3dmPosKey(&R[i * 3])];
                if (std::find(lista.begin(), lista.end(), p) == lista.end()) lista.push_back(p);
            }
    }
    const std::map<std::string, std::vector<int> >& mapaClave = mapaReposo.empty() ? P.porPos : mapaReposo;
    pares.reserve(src.size());
    int perdidas = 0;
    for (std::set<std::string>::const_iterator it = src.begin(); it != src.end(); ++it) {
        if (it->size() != 24) { perdidas++; continue; }
        std::map<std::string, std::vector<int> >::const_iterator a = mapaClave.find(it->substr(0, 12));
        std::map<std::string, std::vector<int> >::const_iterator b = mapaClave.find(it->substr(12, 12));
        // (1) la clave de POSICION ya no cae en ningun punto de la malla: el vertice
        //     se MOVIO o se BORRO. SILENCIOSO JAMAS: perderla esta bien, perderla sin
        //     decirlo no (y el aviso no puede afirmar CUAL de las dos fue).
        if (a == mapaClave.end() || b == mapaClave.end()) { perdidas++; continue; }
        // (2) los puntos existen PERO no hay una arista entre ellos: es la variante
        //     FANTASMA (borrar caras deja los puntos vivos, o un corte parte la arista
        //     en dos). Validar solo las POSICIONES no alcanzaba y la marca se escribia
        //     sobre una arista que no esta.
        //     Con VARIOS puntos en una posicion (vertices distintos en el mismo lugar)
        //     la marca es de TODA arista real entre esas dos posiciones, igual que en
        //     memoria (la clave es de posicion): tomar solo el primer punto perdia la
        //     costura de la arista que usa al otro, y el archivo salia sin ella.
        bool alguna = false;
        for (size_t ia = 0; ia < a->second.size(); ia++)
            for (size_t ib = 0; ib < b->second.size(); ib++) {
                int x = a->second[ia], y = b->second[ib];
                if (x == y) continue;
                if (x > y) { int t = x; x = y; y = t; }
                if (!aristasReales.count(std::make_pair(x, y))) continue;
                pares.push_back(std::make_pair(x, y));
                alguna = true;
            }
        if (!alguna) perdidas++;
    }
    if (perdidas > 0 && avisos) {
        std::string s = "no pude guardar "; W3dEscribirInt(s, perdidas);
        s += (seam ? " costura(s) SEAM" : " borde(s) SHARP");
        s += ": esa arista ya no existe en la malla (se borro o se movio la geometria despues de marcarla)";
        avisos->push_back(s);
    }
    // orden numerico estable (el set viene ordenado por BYTES de float, que
    // no es un orden util para un humano ni estable ante un re-merge).
    // std::sort y no el doble for de antes: era O(n^2) y con 80.000 aristas
    // marcadas se comia varios segundos EN CADA GUARDADO.
    std::sort(pares.begin(), pares.end());
    // la misma arista no se escribe dos veces (una clave (A,B) y otra (B,A) del set
    // llegarian al mismo par): el lector la volveria a juntar y re-guardar cambiaria bytes
    pares.erase(std::unique(pares.begin(), pares.end()), pares.end());
}

void W3dMallaRepDeCp(const Mesh* m, std::map<int, int>& repDeCp) {
    repDeCp.clear();
    if (!m) return;
    const int nV = (m->vertex ? m->vertexSize : 0);
    if ((int)m->vertCtrlPoint.size() == nV)
        for (int i = 0; i < nV; i++) {
            int cp = m->vertCtrlPoint[(size_t)i];
            if (cp >= 0 && repDeCp.find(cp) == repDeCp.end()) repDeCp[cp] = i;
        }
}

bool W3dMallaGrupoPorPunto(const Mesh* m, const W3dMallaPuntos& P, const std::map<int, int>& repDeCp,
                           size_t g, std::vector<int>& pi, std::vector<float>& pw, std::vector<std::string>* avisos) {
    pi.clear(); pw.clear();
    if (!m || g >= m->vertexGroups.size() || !m->vertexGroups[g]) return false;
    const VertexGroup* vg = m->vertexGroups[g];
    const int nV = P.nV;
    // control-point -> punto: el control-point es un indice de render-vert
    // representante (vertCtrlPoint), asi que se traduce por su posicion.
    std::set<int> vistos;
    int choques = 0;
    for (size_t k = 0; k < vg->verts.size() && k < vg->pesos.size(); k++) {
        int cp = vg->verts[k];
        int rv = -1;
        if (!repDeCp.empty()) {
            std::map<int, int>::const_iterator it = repDeCp.find(cp);
            if (it != repDeCp.end()) rv = it->second;
        } else if (cp >= 0 && cp < nV) rv = cp;
        if (rv < 0) continue;
        int p = P.rvAPunto[(size_t)rv];
        if (p < 0) continue;
        // DOS CONTROL-POINTS EN LA MISMA POSICION: en el archivo son UN punto.
        // Es el caso NORMAL de una costura de UV o de color (el vertice se parte
        // pero la posicion es una sola) y ahi los dos pesos son el MISMO valor:
        // no se pierde nada, el punto ya lo representa. Solo hay perdida real
        // cuando los pesos DIFIEREN -- eso pasa con una malla importada de
        // glTF/FBX, donde el control-point lo trae el importador. Gana el
        // primero; perderlo esta bien, perderlo EN SILENCIO no.
        if (!vistos.insert(p).second) {
            float ya = 0.0f;
            for (size_t q = 0; q < pi.size(); q++) if (pi[q] == p) { ya = pw[q]; break; }
            const float d = ya - vg->pesos[k];
            if (d > 1e-6f || d < -1e-6f) choques++;
            continue;
        }
        pi.push_back(p); pw.push_back(vg->pesos[k]);
    }
    if (choques > 0 && avisos) {
        std::string s = "el vertex group '"; s += vg->nombre;
        s += "' perdio "; W3dEscribirInt(s, choques);
        s += " peso(s): son control-points distintos parados en la MISMA posicion y en el archivo son un solo punto";
        avisos->push_back(s);
    }
    return true;
}

void W3dMallaFormaPorPunto(const Mesh* m, const W3dMallaPuntos& P, const std::map<int, int>& repDeCp,
                           size_t k, std::vector<int>& pi, std::vector<float>& pd) {
    pi.clear(); pd.clear();
    if (!m || k >= m->shapeKeys.size()) return;
    const W3dShapeKey& sk = m->shapeKeys[k];
    const int nV = P.nV;
    std::set<int> vistos;
    for (size_t j = 0; j < sk.idx.size() && j * 3 + 2 < sk.d.size(); j++) {
        int cp = sk.idx[j], rv = -1;
        if (!repDeCp.empty()) { std::map<int, int>::const_iterator it = repDeCp.find(cp); if (it != repDeCp.end()) rv = it->second; }
        else if (cp >= 0 && cp < nV) rv = cp;
        if (rv < 0) continue;
        int pt = P.rvAPunto[(size_t)rv];
        if (pt < 0 || !vistos.insert(pt).second) continue;   // mismo punto (costura): el delta es el mismo
        pi.push_back(pt); pd.push_back(sk.d[j*3]); pd.push_back(sk.d[j*3+1]); pd.push_back(sk.d[j*3+2]);
    }
}

bool W3dMallaUVGrupoPorEsquina(const Mesh* m, size_t g, std::vector<int>& ci, std::vector<float>& cw) {
    ci.clear(); cw.clear();
    if (!m || g >= m->uvGroups.size() || !m->uvGroups[g]) return false;
    const UVGroup* ug = m->uvGroups[g];
    const int nV = (m->vertex ? m->vertexSize : 0);
    std::vector<float> w((size_t)(nV > 0 ? nV : 1), 0.0f);
    for (size_t k = 0; k < ug->verts.size() && k < ug->pesos.size(); k++) {
        int rv = ug->verts[k];
        if (rv >= 0 && rv < nV) w[(size_t)rv] = ug->pesos[k];
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

bool W3dMallaEscribir(const Mesh* m, std::string& out, std::vector<std::string>* avisos, int anclarRender) {
    out.clear();
    if (!m) return false;
    // NO SE GUARDA ENCIMA DE UNA MALLA QUE NO ENTENDEMOS. El que llama ya deberia
    // haber avisado con el nombre del bloque (W3dMallaBloqueQueFalta); esto es el
    // cierre, para que ningun camino nuevo se saltee el freno por olvido.
    if (!W3dMallaBloqueQueFalta(m).empty()) {
        if (avisos)
            avisos->push_back("no guardo esta malla: el archivo REQUIERE el bloque '" +
                              W3dMallaBloqueQueFalta(m) + "' y esta version no lo entiende");
        return false;
    }

    // ---- PUNTOS DE CONTROL: la IDENTIDAD de los vertices (ver W3dMallaCalcularPuntos, que
    //      comparte con el .w3db): posicion + datos de punto; la geometria suelta no se suelda.
    W3dMallaPuntos P;
    W3dMallaCalcularPuntos(m, P);
    const int nV = P.nV;
    const std::vector<int>& rvAPunto = P.rvAPunto;
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
    // malla (W3dAjenos::requiere) y solo se pone el default cuando no salio de un
    // archivo (malla nueva del editor).
    if (!m->w3dmAjenos.requiere.empty()) {
        out += "requiere";
        for (size_t i = 0; i < m->w3dmAjenos.requiere.size(); i++) {
            out += ' ';
            out += W3dmNombreSano(m->w3dmAjenos.requiere[i], avisos, "de un bloque de 'requiere'");
        }
        out += "\n";
    } else {
        out += "requiere V F\n";
    }
    out += "nombre "; out += W3dmNombreSano(m->name, avisos, "de la malla"); out += "\n";
    out += "puntos ";   W3dEscribirInt(out, nP); out += "\n";
    out += "esquinas "; W3dEscribirInt(out, nC); out += "\n";
    out += "caras ";    W3dEscribirInt(out, nF); out += "\n";
    // rverts: solo tiene sentido cuando hay que ANCLAR indices de render (vertex anims)
    // (anclarRender: -1 = si la malla tiene vertex anims; 0/1 = lo decide el que llama, ver el .h)
    const bool conAnim = (anclarRender < 0) ? !m->animations.empty() : (anclarRender != 0);
    if (conAnim) { out += "rverts "; W3dEscribirInt(out, nV); out += "\n"; }
    out += "suave ";    W3dEscribirInt(out, m->meshSmooth ? 1 : 0); out += "\n";
    // QUE CAPAS SALEN (y las LAZY: la derivada del render), decidido ANTES de escribir nada.
    // Ver W3dMallaElegirCapas: el mismo criterio (y los mismos avisos) que usa el .w3db.
    W3dMallaCapasSalen capas;
    W3dMallaElegirCapas(m, nC, capas, avisos);
    const bool uvDerivada = capas.uvDerivada, colDerivada = capas.colDerivada;
    const std::vector<size_t>& uvSalen = capas.uv;
    const std::vector<size_t>& colSalen = capas.col;
    const int uvActivaSale = capas.uvActiva, colActivaSale = capas.colActiva;

    if (!m->uvMaps.empty())       { out += "uvactiva ";   W3dEscribirInt(out, uvActivaSale);  out += "\n"; }
    else if (uvDerivada)          { out += "uvactiva 0\n"; }
    if (!m->colorLayers.empty())  { out += "colactiva ";  W3dEscribirInt(out, colActivaSale);  out += "\n"; }
    else if (colDerivada)         { out += "colactiva 0\n"; }
    if (!m->vertexGroups.empty()) { out += "vgactiva ";   W3dEscribirInt(out, m->grupoActivo);  out += "\n"; }
    if (!m->uvGroups.empty())     { out += "uvgactiva ";  W3dEscribirInt(out, m->uvGrupoActivo); out += "\n"; }
    if (m->meshTipo >= 0) {
        out += "prim ";
        if (m->meshTipo < kPrimN) out += kPrim[m->meshTipo]; else W3dEscribirInt(out, m->meshTipo);
        out += ' '; W3dEscribirFloat(out, m->meshSize);
        out += ' '; W3dEscribirFloat(out, m->meshSize2);
        out += ' '; W3dEscribirFloat(out, m->meshDepth);
        out += ' '; W3dEscribirInt(out, m->meshVerts);
        out += ' '; W3dEscribirInt(out, m->meshVerts2);
        out += "\n";
    }

    // ---- V: los PUNTOS de control ---------------------------------------
    out += "\n# los PUNTOS de control (x y z)\n";
    CabBloque(out, "V", nP, W3DM_PUNTO); out += "\n";
    for (int i = 0; i < nP; i++) {
        out += ' '; W3dEscribirFloat(out, pts[(size_t)i * 3]);
        out += ' '; W3dEscribirFloat(out, pts[(size_t)i * 3 + 1]);
        out += ' '; W3dEscribirFloat(out, pts[(size_t)i * 3 + 2]);
        out += "\n";
    }
    out += "END\n";

    // ---- F: caras NATIVAS (tri / quad / ngon, sin triangular) -----------
    out += "\n# caras: <lados> y sus puntos, en orden alrededor del poligono\n";
    CabBloque(out, "F", nF, W3DM_CARA); out += "\n";
    for (int f = 0; f < nF; f++) {
        const MeshFace& F = m->faces3d[(size_t)f];
        out += ' '; W3dEscribirInt(out, (int)F.idx.size());
        for (size_t c = 0; c < F.idx.size(); c++) {
            int gv = F.idx[c];
            int pi = (gv >= 0 && gv < nV) ? rvAPunto[(size_t)gv] : -1;
            out += ' '; W3dEscribirInt(out, pi);
        }
        out += "\n";
    }
    out += "END\n";

    // ---- FPART / FSHADE: SPARSE (lo que no aporta nada NO se escribe) ----
    {
        int n = 0;
        for (int f = 0; f < nF; f++) if (m->faces3d[(size_t)f].mat != 0) n++;
        if (n > 0) {
            out += "\n"; CabBloque(out, "FPART", n, W3DM_CARA); out += "\n";
            for (int f = 0; f < nF; f++) if (m->faces3d[(size_t)f].mat != 0) {
                out += ' '; W3dEscribirInt(out, f);
                out += ' '; W3dEscribirInt(out, m->faces3d[(size_t)f].mat); out += "\n";
            }
            out += "END\n";
        }
    }
    {
        int n = 0;
        for (int f = 0; f < nF; f++) if (m->faces3d[(size_t)f].smooth >= 0) n++;
        if (n > 0) {
            out += "\n"; CabBloque(out, "FSHADE", n, W3DM_CARA); out += "\n";
            for (int f = 0; f < nF; f++) if (m->faces3d[(size_t)f].smooth >= 0) {
                out += ' '; W3dEscribirInt(out, f);
                out += ' '; W3dEscribirInt(out, m->faces3d[(size_t)f].smooth); out += "\n";
            }
            out += "END\n";
        }
    }

    // ---- NRM: normal por esquina (dato DEL USUARIO, no derivado) --------
    //  LAS CAPAS SON LAZY EN MEMORIA: una malla recien creada (o importada) todavia NO tiene
    //  cornerNormal ni uvMaps ni colorLayers; sus datos viven solo en los arrays de RENDER hasta
    //  que alguien entra al editor y PoblarCapas() los materializa. El archivo NO puede depender
    //  de eso: si no hay capa se emite la version DERIVADA del render (corner L -> faces3d[f].idx[c],
    //  que es exactamente el mapeo que usa PoblarCapas). Sin esto, guardar un cubo recien agregado
    //  perdia sus UV y sus normales, y al reabrir el merge colapsaba los 24 render-verts en 8.
    //  Al leer, esa version derivada vuelve como una capa de verdad -> el re-guardado es identico.
    if (nC > 0 && ((int)m->cornerNormal.size() == nC * 3 || m->normals)) {
        const bool porCapa = ((int)m->cornerNormal.size() == nC * 3);
        out += "\n# normal por esquina (x y z enteros, escala 1/127)\n";
        CabBloque(out, "NRM", nC, W3DM_ESQUINA); out += "\n";
        int L = 0;
        for (size_t f = 0; f < m->faces3d.size(); f++)
            for (size_t c = 0; c < m->faces3d[f].idx.size(); c++, L++) {
                int gv = m->faces3d[f].idx[c];
                const GLbyte* n3;
                GLbyte cero[3] = { 0, 127, 0 };
                if (porCapa) n3 = &m->cornerNormal[(size_t)L * 3];
                else if (gv >= 0 && gv < nV) n3 = &m->normals[(size_t)gv * 3];
                else n3 = cero;
                out += ' '; W3dEscribirInt(out, (int)n3[0]);
                out += ' '; W3dEscribirInt(out, (int)n3[1]);
                out += ' '; W3dEscribirInt(out, (int)n3[2]);
                out += "\n";
            }
        out += "END\n";
    }

    // ---- UV: UNA por capa, en el orden de Mesh::uvMaps ------------------
    if (!m->uvMaps.empty()) {
        for (size_t k = 0; k < uvSalen.size(); k++) {
            const UVMap* u = m->uvMaps[uvSalen[k]];
            out += "\n"; CabBloque(out, "UV", nC, W3DM_ESQUINA);
            out += " nombre="; out += W3dmNombreSano(u->nombre, avisos, "de una capa UV"); out += "\n";
            for (int i = 0; i < nC; i++) {
                out += ' '; W3dEscribirFloat(out, u->uv[(size_t)i * 2]);
                out += ' '; W3dEscribirFloat(out, u->uv[(size_t)i * 2 + 1]);
                out += "\n";
            }
            out += "END\n";
        }
    } else if (uvDerivada) {
        // sin capa todavia: la UV derivada del render, con el MISMO nombre que le pondria
        // PoblarCapas ("UVMap"), para que leer y volver a guardar de los mismos bytes.
        out += "\n"; CabBloque(out, "UV", nC, W3DM_ESQUINA); out += " nombre=UVMap\n";
        for (size_t f = 0; f < m->faces3d.size(); f++)
            for (size_t c = 0; c < m->faces3d[f].idx.size(); c++) {
                int gv = m->faces3d[f].idx[c];
                float u0 = 0.0f, v0 = 0.0f;
                if (gv >= 0 && gv < nV) { u0 = m->uv[(size_t)gv * 2]; v0 = m->uv[(size_t)gv * 2 + 1]; }
                out += ' '; W3dEscribirFloat(out, u0);
                out += ' '; W3dEscribirFloat(out, v0);
                out += "\n";
            }
        out += "END\n";
    }

    // ---- COL: capas de COLOR POR VERTICE. HOY ESTO SE PIERDE ENTERO -----
    //      (el exportador GLB nunca emitio COLOR_0 y el JSON no las guarda).
    //      El almacenamiento es SIEMPRE por esquina aunque el flag porVertice
    //      este puesto, porque el toggle del editor es NO DESTRUCTIVO: en el
    //      archivo el flag es una CLAVE, no un cambio de layout.
    for (size_t k = 0; k < colSalen.size(); k++) {
        const ColorLayer* c = m->colorLayers[colSalen[k]];
        out += "\n"; CabBloque(out, "COL", nC, W3DM_ESQUINA);
        if (c->porVertice) out += " por=vertice";
        out += " nombre="; out += W3dmNombreSano(c->nombre, avisos, "de una capa de color"); out += "\n";
        for (int i = 0; i < nC; i++) {
            out += ' '; W3dEscribirInt(out, (int)c->color[(size_t)i * 4]);
            out += ' '; W3dEscribirInt(out, (int)c->color[(size_t)i * 4 + 1]);
            out += ' '; W3dEscribirInt(out, (int)c->color[(size_t)i * 4 + 2]);
            out += ' '; W3dEscribirInt(out, (int)c->color[(size_t)i * 4 + 3]);
            out += "\n";
        }
        out += "END\n";
    }
    if (colDerivada) {
        // idem UV: el color por vertice de un .obj importado vive solo en el array de render
        // hasta que alguien abre el editor. Nombre "Col", el mismo que le pondria PoblarCapas.
        // Todo blanco NO se escribe: no aporta nada y engordaria todos los archivos.
        out += "\n"; CabBloque(out, "COL", nC, W3DM_ESQUINA); out += " nombre=Col\n";
        for (size_t f = 0; f < m->faces3d.size(); f++)
            for (size_t c = 0; c < m->faces3d[f].idx.size(); c++) {
                int gv = m->faces3d[f].idx[c];
                for (int k = 0; k < 4; k++) {
                    int v = (gv >= 0 && gv < nV) ? (int)m->vertexColor[(size_t)gv * 4 + (size_t)k] : 255;
                    out += ' '; W3dEscribirInt(out, v);
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
    //  presente): el armature vive en el proyecto.json y no viaja en el .w3dm,
    //  asi que condicionarlo a el haria que el rest se perdiera al copiar la
    //  malla sola, y ademas rompe el round-trip byte a byte.
    if ((int)m->uv2dRest.size() == nV * 2 && nV > 0 && nC > 0) {
        out += "\n# UV de REST del skinning 2D (por esquina; se colapsa al leer)\n";
        CabBloque(out, "UVREST", nC, W3DM_ESQUINA); out += "\n";
        for (size_t f = 0; f < m->faces3d.size(); f++)
            for (size_t c = 0; c < m->faces3d[f].idx.size(); c++) {
                int gv = m->faces3d[f].idx[c];
                float u = 0.0f, v = 0.0f;
                if (gv >= 0 && gv < nV) { u = m->uv2dRest[(size_t)gv * 2]; v = m->uv2dRest[(size_t)gv * 2 + 1]; }
                out += ' '; W3dEscribirFloat(out, u);
                out += ' '; W3dEscribirFloat(out, v);
                out += "\n";
            }
        out += "END\n";
    }

    // ---- ARISTAS REALES (pares de PUNTOS, sin repetir) de F + EDGE. Las usan
    //      dos cosas: la validacion de SHARP/SEAM (abajo) y el conteo del dominio
    //      W3DM_ARISTA de los bloques ajenos (mas abajo). Se arma UNA sola vez y
    //      solo si alguna de las dos la necesita.
    std::set<std::pair<int, int> > aristasReales;
    const bool hayMarcas = (!m->sharpEdges.empty() || !m->seamEdges.empty());
    if (hayMarcas || !m->w3dmAjenos.bloques.empty()) W3dMallaAristasReales(m, P, aristasReales);

    // ---- SHARP / SEAM: en memoria son sets con clave de POSICION (no hay
    //      indice de arista en ningun lado); en el archivo van como PAR DE
    //      PUNTOS y al cargar se rearma la clave desde las posiciones leidas.
    //      La validacion (y el invariante de POSE) vive en W3dMallaMarcasValidas.
    for (int pasada = 0; pasada < 2; pasada++) {
        std::vector<std::pair<int, int> > pares;
        W3dMallaMarcasValidas(m, P, aristasReales, pasada != 0, pares, avisos);
        if (pares.empty()) continue;
        out += "\n"; CabBloque(out, pasada ? "SEAM" : "SHARP", (int)pares.size(), W3DM_ARISTA); out += "\n";
        for (size_t i = 0; i < pares.size(); i++) {
            out += ' '; W3dEscribirInt(out, pares[i].first);
            out += ' '; W3dEscribirInt(out, pares[i].second); out += "\n";
        }
        out += "END\n";
    }

    // ---- EDGE / POINT: geometria SUELTA (hoy se pierde entera).
    //      EL ORDEN ES SIGNIFICATIVO: de el depende la numeracion de los
    //      render-verts sueltos al recargar.
    if (m->looseEdges.size() >= 2) {
        int n = (int)(m->looseEdges.size() / 2);
        out += "\n# geometria SUELTA (aristas y vertices sin cara)\n";
        CabBloque(out, "EDGE", n, W3DM_ARISTA); out += "\n";
        for (size_t i = 0; i + 1 < m->looseEdges.size(); i += 2) {
            int a = m->looseEdges[i], b = m->looseEdges[i + 1];
            out += ' '; W3dEscribirInt(out, (a >= 0 && a < nV) ? rvAPunto[(size_t)a] : -1);
            out += ' '; W3dEscribirInt(out, (b >= 0 && b < nV) ? rvAPunto[(size_t)b] : -1);
            out += "\n";
        }
        out += "END\n";
    }
    if (!m->looseVerts.empty()) {
        out += "\n"; CabBloque(out, "POINT", (int)m->looseVerts.size(), W3DM_PUNTO); out += "\n";
        for (size_t i = 0; i < m->looseVerts.size(); i++) {
            int a = m->looseVerts[i];
            out += ' '; W3dEscribirInt(out, (a >= 0 && a < nV) ? rvAPunto[(size_t)a] : -1); out += "\n";
        }
        out += "END\n";
    }

    // ---- PARTS / PARTMAT: de MaterialGroup SOLO el nombre y el vinculo al
    //      material son DATOS; start/count los rehace ReagruparMeshParts.
    if (!m->materialsGroup.empty()) {
        out += "\n# partes de malla (el material vive en la escena: se referencia POR NOMBRE)\n";
        CabBloque(out, "PARTS", (int)m->materialsGroup.size(), W3DM_GLOBAL); out += "\n";
        for (size_t i = 0; i < m->materialsGroup.size(); i++) {
            // ESCAPE: un mesh part llamado "END" cerraria el bloque (ver W3dmEscaparPayload)
            out += ' ';
            out += W3dmEscaparPayload(W3dmNombreSano(m->materialsGroup[i].name, avisos, "de un mesh part"));
            out += "\n";
        }
        out += "END\n";
        int n = 0;
        for (size_t i = 0; i < m->materialsGroup.size(); i++)
            if (m->materialsGroup[i].material && !m->materialsGroup[i].material->name.empty()) n++;
        if (n > 0) {
            out += "\n"; CabBloque(out, "PARTMAT", n, W3DM_GLOBAL); out += "\n";
            for (size_t i = 0; i < m->materialsGroup.size(); i++)
                if (m->materialsGroup[i].material && !m->materialsGroup[i].material->name.empty()) {
                    out += ' '; W3dEscribirInt(out, (int)i);
                    out += ' '; out += W3dmNombreSano(m->materialsGroup[i].material->name, avisos, "de un material"); out += "\n";
                }
            out += "END\n";
        }
    }

    // ---- VG: vertex groups (pesos por CONTROL-POINT = por PUNTO). Hoy se
    //      guardan por la POSICION del punto porque el GLB re-splitea y los
    //      indices no sobreviven; aca los INDICES SON el dato.
    // control-point -> PRIMER render-vert que lo usa (se arma UNA vez, no por grupo)
    std::map<int, int> repDeCp;
    W3dMallaRepDeCp(m, repDeCp);
    for (size_t g = 0; g < m->vertexGroups.size(); g++) {
        // el pasaje control-point -> punto (y el aviso de los pesos que chocan) es
        // W3dMallaGrupoPorPunto, el mismo que usa el .w3db
        std::vector<int> pi;
        std::vector<float> pw;
        if (!W3dMallaGrupoPorPunto(m, P, repDeCp, g, pi, pw, avisos)) continue;
        const VertexGroup* vg = m->vertexGroups[g];
        out += "\n"; CabBloque(out, "VG", (int)pi.size(), W3DM_PUNTO);
        out += " nombre="; out += W3dmNombreSano(vg->nombre, avisos, "de un vertex group"); out += "\n";
        for (size_t k = 0; k < pi.size(); k++) {
            out += ' '; W3dEscribirInt(out, pi[k]);
            out += ' '; W3dEscribirFloat(out, pw[k]); out += "\n";
        }
        out += "END\n";
    }

    // ---- SHAPE: shape keys (formas de la malla: la cara que habla). Esparsas POR PUNTO, con el MISMO pasaje
    //      control-point -> punto que los vertex groups (repDeCp + rvAPunto): "p dx dy dz" por linea. Los pesos NO
    //      se guardan aca (los anima la escena, canal AnimShape).
    for (size_t k = 0; k < m->shapeKeys.size(); k++) {
        const W3dShapeKey& sk = m->shapeKeys[k];
        std::vector<int> pi; std::vector<float> pd;
        W3dMallaFormaPorPunto(m, P, repDeCp, k, pi, pd);
        out += "\n"; CabBloque(out, "SHAPE", (int)pi.size(), W3DM_PUNTO);
        out += " nombre="; out += W3dmNombreSano(sk.nombre, avisos, "de una shape key"); out += "\n";
        for (size_t q = 0; q < pi.size(); q++) {
            out += ' '; W3dEscribirInt(out, pi[q]);
            out += ' '; W3dEscribirFloat(out, pd[q*3]);
            out += ' '; W3dEscribirFloat(out, pd[q*3+1]);
            out += ' '; W3dEscribirFloat(out, pd[q*3+2]); out += "\n";
        }
        out += "END\n";
    }

    // ---- UVG: UV groups. En memoria son por RENDER-VERT; en el archivo van
    //      POR ESQUINA y se colapsan al leer (lossless: todas las esquinas de un
    //      render-vert comparten el peso por construccion).
    for (size_t g = 0; g < m->uvGroups.size(); g++) {
        std::vector<int> ci; std::vector<float> cw;
        if (!W3dMallaUVGrupoPorEsquina(m, g, ci, cw)) continue;
        const UVGroup* ug = m->uvGroups[g];
        out += "\n"; CabBloque(out, "UVG", (int)ci.size(), W3DM_ESQUINA);
        out += " nombre="; out += W3dmNombreSano(ug->nombre, avisos, "de un UV group"); out += "\n";
        for (size_t k = 0; k < ci.size(); k++) {
            out += ' '; W3dEscribirInt(out, ci[k]);
            out += ' '; W3dEscribirFloat(out, cw[k]); out += "\n";
        }
        out += "END\n";
    }

    // ---- RVMAP: CACHE, no requisito. Se escribe SOLO cuando la malla tiene
    //      vertex animations (que es donde el indice de render-vert es dato de
    //      verdad, porque los keyframes estan indexados asi). En el 99% de las
    //      mallas el archivo queda limpio y el lector rehace el merge.
    if (conAnim && nC > 0) {
        out += "\n# render-vert de cada esquina (ancla de las vertex anims)\n";
        CabBloque(out, "RVMAP", nC, W3DM_ESQUINA);
        out += " rverts="; W3dEscribirInt(out, nV); out += "\n";
        int col = 0;
        for (size_t f = 0; f < m->faces3d.size(); f++)
            for (size_t c = 0; c < m->faces3d[f].idx.size(); c++) {
                if (col == 0) out += ' ';
                W3dEscribirInt(out, m->faces3d[f].idx[c]);
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
    if (!m->w3dmAjenos.bloquesBin.empty() && avisos) {
        std::string s2 = "no pude conservar ";
        W3dEscribirInt(s2, (int)m->w3dmAjenos.bloquesBin.size());
        s2 += " bloque(s) binario(s) de una version mas nueva de Whisk3D (";
        for (size_t i = 0; i < m->w3dmAjenos.bloquesBin.size(); i++) {
            if (i) s2 += " ";
            s2 += m->w3dmAjenos.bloquesBin[i].nombre;
        }
        s2 += "): el formato de texto no los puede llevar; guarda en binario para conservarlos";
        avisos->push_back(s2);
    }
    if (!m->w3dmAjenos.bloques.empty()) {
        // cantidad de aristas UNICAS (pares de puntos sin repetir) de F + EDGE:
        // es el MISMO set que valida SHARP/SEAM, armado una sola vez mas arriba.
        const int nA = (int)aristasReales.size();
        bool alguno = false;
        for (size_t i = 0; i < m->w3dmAjenos.bloques.size(); i++) {
            const W3dBloqueAjeno& b = m->w3dmAjenos.bloques[i];
            int ahora = -1, antes = -1;
            switch (b.dominio) {
                case W3DM_PUNTO:   ahora = nP; antes = m->w3dmAjenos.nPuntos;   break;
                case W3DM_ESQUINA: ahora = nC; antes = m->w3dmAjenos.nEsquinas; break;
                case W3DM_CARA:    ahora = nF; antes = m->w3dmAjenos.nCaras;    break;
                case W3DM_ARISTA:  ahora = nA; antes = m->w3dmAjenos.nAristas;  break;
                default: break; // global: siempre
            }
            if (b.dominio != W3DM_GLOBAL && ahora != antes) {
                if (avisos) {
                    std::string s = "no pude conservar el bloque " + b.nombre + " (";
                    W3dEscribirInt(s, antes); s += " -> "; W3dEscribirInt(s, ahora); s += " ";
                    s += W3dmDomNombre(b.dominio);
                    s += "): lo trajo una version mas nueva de Whisk3D y cambiaste la topologia";
                    avisos->push_back(s);
                }
                continue;
            }
            if (!alguno) { out += "\n# bloques de una version mas nueva de Whisk3D\n"; alguno = true; }
            out += b.texto;
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

struct CapaCorner {   // una capa UV o COL leida del archivo
    std::string nombre;
    bool porVertice;
    std::vector<float>         uv;
    std::vector<unsigned char> col;
    CapaCorner() : porVertice(false) {}
};
struct GrupoLeido {
    std::string nombre;
    std::vector<int>   idx;
    std::vector<float> peso;
    std::vector<float> d;      // SHAPE: 3 por idx (dx, dy, dz)
};

// cabecera de bloque ya parseada
struct Cab {
    std::string nombre;
    int cantidad;
    int dominio;
    std::string etiqueta;   // valor de nombre=
    bool porVertice;        // clave por=vertice
    int  rvertsClave;       // clave rverts=
    Cab() : cantidad(0), dominio(W3DM_DOMMAL), porVertice(false), rvertsClave(-1) {}
};

bool ParsearCab(const char* p, const char* fin, Cab& out) {
    if (!W3dLeerPalabra(p, fin, out.nombre)) return false;
    if (!W3dLeerEntero(p, fin, out.cantidad)) return false;
    std::string dom;
    if (!W3dLeerPalabra(p, fin, dom)) return false;
    out.dominio = W3dmDomDe(dom);
    if (out.dominio == W3DM_DOMMAL) return false;
    // extras: clave=valor. "nombre=" va ULTIMO y toma el RESTO de la linea.
    for (;;) {
        W3dSaltarBlancos(p, fin);
        if (p >= fin) break;
        if ((size_t)(fin - p) >= 7 && memcmp(p, "nombre=", 7) == 0) {
            // TAL CUAL: el nombre arranca justo despues del '=' y llega hasta el
            // final de la linea, bordes incluidos (ver W3dNombreLinea).
            out.etiqueta.assign(p + 7, (size_t)(fin - (p + 7)));
            break;
        }
        std::string kv;
        if (!W3dLeerPalabra(p, fin, kv)) break;
        if (kv == "por=vertice") out.porVertice = true;
        else if (kv.size() > 7 && kv.compare(0, 7, "rverts=") == 0) {
            const char* q = kv.c_str() + 7;
            const char* qf = kv.c_str() + kv.size();
            int v = 0; if (W3dLeerEntero(q, qf, v)) out.rvertsClave = v;
        }
        // una clave que no conocemos se IGNORA (es como se agregan campos nuevos)
    }
    if (out.cantidad < 0) out.cantidad = 0;
    return true;
}

} // namespace

bool W3dMallaLeerTexto(const std::string& txt, Mesh* m, W3dMallaInfo* info) {
    return W3dMallaLeer(txt.empty() ? "" : txt.data(), txt.size(), m, info);
}

bool W3dMallaLeer(const char* datos, size_t n, Mesh* m, W3dMallaInfo* info) {
    if (!m || !datos) return false;
    W3dLineas lin(datos, n);
    const char* li; const char* lf;

    // ---- CABECERA: "W3DMESH <version del LEXICO>". Agregar bloques NUNCA la
    //      sube; un lector que ve una version mayor SE NIEGA y lo dice.
    if (!lin.Siguiente(li, lf)) return false;
    {
        const char* p = li;
        std::string w;
        if (!W3dLeerPalabra(p, lf, w) || w != "W3DMESH") return false;
        int ver = 0;
        if (!W3dLeerEntero(p, lf, ver)) ver = 0;
        if (ver > 1) {
            W3dmAviso(info, 1, "este .w3dm es de una version del formato mas nueva; no lo puedo leer");
            return false;
        }
    }

    // ---- acumuladores ---------------------------------------------------
    std::string nombre;
    int declPuntos = -1, declEsquinas = -1, declCaras = -1, declRverts = -1;
    int suave = 1, uvActiva = -1, colActiva = -1, vgActiva = -1, uvgActiva = -1;
    int primTipo = -1; float primS = 2.0f, primS2 = 0.0f, primD = 0.0f; int primV = 8, primV2 = 8;
    bool haySuave = false, hayPrim = false;

    std::vector<float> pts;
    std::vector<int>   carasLados, carasIdx;
    std::vector<int>   fpartF, fpartV, fshadeF, fshadeV;
    std::vector<signed char> nrm;
    std::vector<CapaCorner>  capasUV, capasCol;
    std::vector<float> uvrest;
    std::vector<int>   sharp, seam, edgeSuelta, puntoSuelto;
    std::vector<std::string> parts;
    std::vector<int> partmatI; std::vector<std::string> partmatN;
    std::vector<GrupoLeido> vgs, uvgs, formas;
    std::vector<int> rvmap; int rvmapRverts = -1;
    std::vector<W3dBloqueAjeno> ajenos;
    std::vector<std::string> requiere;
    // los DOS bloques que el propio archivo declara obligatorios ("requiere V F").
    // Que aparezcan es lo unico que distingue "una malla vacia" de "el archivo se
    // corto" (ver el chequeo de abajo).
    bool vioV = false, vioF = false;

    // ---- recorrido por lineas ------------------------------------------
    while (lin.Siguiente(li, lf)) {
        if (li >= lf) continue;                 // linea vacia
        char c0 = *li;
        if (c0 == '#') continue;                // comentario
        if (c0 == ' ' || c0 == '\t') continue;  // dato sin bloque abierto: se ignora

        if (c0 >= 'a' && c0 <= 'z') {           // ---------------- DIRECTIVA
            const char* p = li;
            std::string k;
            W3dLeerPalabra(p, lf, k);
            if      (k == "nombre")   nombre = W3dNombreLinea(p, lf);
            else if (k == "puntos")   W3dLeerEntero(p, lf, declPuntos);
            else if (k == "esquinas") W3dLeerEntero(p, lf, declEsquinas);
            else if (k == "caras")    W3dLeerEntero(p, lf, declCaras);
            else if (k == "rverts")   W3dLeerEntero(p, lf, declRverts);
            else if (k == "suave")   { W3dLeerEntero(p, lf, suave); haySuave = true; }
            else if (k == "uvactiva")  W3dLeerEntero(p, lf, uvActiva);
            else if (k == "colactiva") W3dLeerEntero(p, lf, colActiva);
            else if (k == "vgactiva")  W3dLeerEntero(p, lf, vgActiva);
            else if (k == "uvgactiva") W3dLeerEntero(p, lf, uvgActiva);
            else if (k == "requiere") {
                std::string b;
                while (W3dLeerPalabra(p, lf, b)) requiere.push_back(b);
            } else if (k == "prim") {
                std::string t;
                if (W3dLeerPalabra(p, lf, t)) {
                    primTipo = -1;
                    for (int i = 0; i < kPrimN; i++) if (t == kPrim[i]) { primTipo = i; break; }
                    if (primTipo < 0) {
                        const char* q = t.c_str(); const char* qf = q + t.size();
                        if (!W3dLeerEntero(q, qf, primTipo)) primTipo = -1;
                    }
                    W3dLeerFloat(p, lf, primS); W3dLeerFloat(p, lf, primS2); W3dLeerFloat(p, lf, primD);
                    W3dLeerEntero(p, lf, primV); W3dLeerEntero(p, lf, primV2);
                    hayPrim = (primTipo >= 0);
                }
            }
            // una directiva desconocida se IGNORA a proposito (es como se
            // agregan campos nuevos sin romper a los lectores viejos)
            continue;
        }

        if (!(c0 >= 'A' && c0 <= 'Z')) continue; // basura: se ignora

        // ---------------- BLOQUE ----------------
        Cab cab;
        const int lineaCab = lin.Nro();
        if (!ParsearCab(li, lf, cab)) {
            W3dmAviso(info, lineaCab, "cabecera de bloque mal formada; salteo el bloque");
            // igual hay que consumir hasta END para no descolocar el resto
            while (lin.Siguiente(li, lf)) if (W3dmEsEnd(li, lf)) break;
            continue;
        }
        const bool conocido = W3dMallaBloqueConocido(cab.nombre);

        // --- bloque DESCONOCIDO: se guarda VERBATIM (cabecera .. END) ---
        if (!conocido) {
            W3dBloqueAjeno aj;
            aj.nombre = cab.nombre;
            aj.dominio = cab.dominio;
            aj.cantidad = cab.cantidad;
            aj.texto.assign(li, (size_t)(lf - li));
            aj.texto += "\n";
            bool cerro = false;
            while (lin.Siguiente(li, lf)) {
                aj.texto.append(li, (size_t)(lf - li));
                aj.texto += "\n";
                if (W3dmEsEnd(li, lf)) { cerro = true; break; }
            }
            if (!cerro) { if (info) info->truncado = true; W3dmAviso(info, lineaCab, "bloque sin END (archivo truncado)"); }
            ajenos.push_back(aj);
            continue;
        }

        // --- bloque CONOCIDO: se lee el payload linea por linea ---
        if      (cab.nombre == "V") vioV = true;
        else if (cab.nombre == "F") vioF = true;
        const size_t queda = lin.Restante();
        int cnt = cab.cantidad;
        // reserva ACOTADA por el tamano real que queda del archivo
        if      (cab.nombre == "V")      { pts.reserve((size_t)W3dmAcotar(cnt, queda, 3) * 3); }
        else if (cab.nombre == "F")      { carasLados.reserve((size_t)W3dmAcotar(cnt, queda, 4)); }
        else if (cab.nombre == "NRM")    { nrm.reserve((size_t)W3dmAcotar(cnt, queda, 3) * 3); }

        CapaCorner capa; capa.nombre = cab.etiqueta; capa.porVertice = cab.porVertice;
        GrupoLeido grupo; grupo.nombre = cab.etiqueta;
        if (cab.nombre == "UV")  capa.uv.reserve((size_t)W3dmAcotar(cnt, queda, 2) * 2);
        if (cab.nombre == "COL") capa.col.reserve((size_t)W3dmAcotar(cnt, queda, 4) * 4);
        if (cab.nombre == "RVMAP") { rvmap.reserve((size_t)W3dmAcotar(cnt, queda, 1)); rvmapRverts = cab.rvertsClave; }

        int leidos = 0;
        bool cerro = false;
        while (lin.Siguiente(li, lf)) {
            if (W3dmEsEnd(li, lf)) { cerro = true; break; }
            if (li >= lf) continue;
            if (*li == '#') continue;
            if (!(*li == ' ' || *li == '\t')) {
                // otra cabecera/directiva ANTES del END: el bloque estaba mal cerrado.
                W3dmAviso(info, lin.Nro(), "bloque sin END; lo cierro aca");
                cerro = true;
                break;
            }
            const char* p = li;
            if (cab.nombre == "V") {
                float x = 0, y = 0, z = 0;
                W3dLeerFloat(p, lf, x); W3dLeerFloat(p, lf, y); W3dLeerFloat(p, lf, z);
                pts.push_back(x); pts.push_back(y); pts.push_back(z);
            } else if (cab.nombre == "F") {
                int lados = 0;
                if (!W3dLeerEntero(p, lf, lados)) continue;
                if (lados < 3 || lados > 100000) { W3dmAviso(info, lin.Nro(), "cara con cantidad de lados invalida; la descarto"); continue; }
                std::vector<int> anillo;
                // ACOTADO CONTRA LO QUE QUEDA DEL ARCHIVO, igual que todas las demas
                // reservas: "100000" por linea son 400 KB, y un archivo malo con
                // 200.000 lineas asi pedia 400 KB CADA UNA. En Symbian eso es un
                // bad_alloc sobre un heap fragmentado (y aca no hay try/catch).
                anillo.reserve((size_t)W3dmAcotar(lados, (size_t)(lf - li), 1));
                bool ok = true;
                for (int i = 0; i < lados; i++) {
                    int v = 0;
                    if (!W3dLeerEntero(p, lf, v)) { ok = false; break; }
                    anillo.push_back(v);
                }
                if (!ok) { W3dmAviso(info, lin.Nro(), "cara con menos indices que lados; la descarto"); continue; }
                carasLados.push_back(lados);
                for (int i = 0; i < lados; i++) carasIdx.push_back(anillo[(size_t)i]);
            } else if (cab.nombre == "FPART" || cab.nombre == "FSHADE") {
                int a = 0, b = 0;
                if (!W3dLeerEntero(p, lf, a) || !W3dLeerEntero(p, lf, b)) continue;
                if (cab.nombre == "FPART") { fpartF.push_back(a); fpartV.push_back(b); }
                else                       { fshadeF.push_back(a); fshadeV.push_back(b); }
            } else if (cab.nombre == "NRM") {
                int x = 0, y = 0, z = 0;
                W3dLeerEntero(p, lf, x); W3dLeerEntero(p, lf, y); W3dLeerEntero(p, lf, z);
                if (x < -128) x = -128; if (x > 127) x = 127;
                if (y < -128) y = -128; if (y > 127) y = 127;
                if (z < -128) z = -128; if (z > 127) z = 127;
                nrm.push_back((signed char)x); nrm.push_back((signed char)y); nrm.push_back((signed char)z);
            } else if (cab.nombre == "UV") {
                float u = 0, v = 0;
                W3dLeerFloat(p, lf, u); W3dLeerFloat(p, lf, v);
                capa.uv.push_back(u); capa.uv.push_back(v);
            } else if (cab.nombre == "UVREST") {
                float u = 0, v = 0;
                W3dLeerFloat(p, lf, u); W3dLeerFloat(p, lf, v);
                uvrest.push_back(u); uvrest.push_back(v);
            } else if (cab.nombre == "COL") {
                int r = 255, g = 255, b = 255, a = 255;
                W3dLeerEntero(p, lf, r); W3dLeerEntero(p, lf, g); W3dLeerEntero(p, lf, b); W3dLeerEntero(p, lf, a);
                if (r < 0) r = 0; if (r > 255) r = 255;
                if (g < 0) g = 0; if (g > 255) g = 255;
                if (b < 0) b = 0; if (b > 255) b = 255;
                if (a < 0) a = 0; if (a > 255) a = 255;
                capa.col.push_back((unsigned char)r); capa.col.push_back((unsigned char)g);
                capa.col.push_back((unsigned char)b); capa.col.push_back((unsigned char)a);
            } else if (cab.nombre == "SHARP" || cab.nombre == "SEAM" || cab.nombre == "EDGE") {
                int a = 0, b = 0;
                if (!W3dLeerEntero(p, lf, a) || !W3dLeerEntero(p, lf, b)) continue;
                std::vector<int>& dst = (cab.nombre == "SHARP") ? sharp : (cab.nombre == "SEAM" ? seam : edgeSuelta);
                dst.push_back(a); dst.push_back(b);
            } else if (cab.nombre == "POINT") {
                int a = 0;
                if (!W3dLeerEntero(p, lf, a)) continue;
                puntoSuelto.push_back(a);
            } else if (cab.nombre == "PARTS") {
                parts.push_back(W3dmDesescaparPayload(W3dNombreLinea(p, lf)));
            } else if (cab.nombre == "PARTMAT") {
                int a = 0;
                if (!W3dLeerEntero(p, lf, a)) continue;
                partmatI.push_back(a); partmatN.push_back(W3dNombreLinea(p, lf));
            } else if (cab.nombre == "VG" || cab.nombre == "UVG") {
                int a = 0; float w = 0;
                if (!W3dLeerEntero(p, lf, a)) continue;
                W3dLeerFloat(p, lf, w);
                grupo.idx.push_back(a); grupo.peso.push_back(w);
            } else if (cab.nombre == "SHAPE") {
                int a = 0; float dx = 0, dy = 0, dz = 0;
                if (!W3dLeerEntero(p, lf, a)) continue;
                W3dLeerFloat(p, lf, dx); W3dLeerFloat(p, lf, dy); W3dLeerFloat(p, lf, dz);
                grupo.idx.push_back(a); grupo.d.push_back(dx); grupo.d.push_back(dy); grupo.d.push_back(dz);
            } else if (cab.nombre == "RVMAP") {
                int a = 0;
                while (W3dLeerEntero(p, lf, a)) rvmap.push_back(a);
            }
            leidos++;
        }
        if (!cerro) { if (info) info->truncado = true; W3dmAviso(info, lineaCab, "bloque sin END (archivo truncado): uso lo que hay"); }
        // TOLERANCIA para el que edita a mano: la cantidad es para RESERVAR, no
        // un contrato. Si sobran o faltan lineas se usa lo que hay y se avisa.
        if (cab.nombre != "RVMAP" && cnt != leidos) {
            std::string s = "bloque "; s += cab.nombre; s += ": declaraba ";
            W3dEscribirInt(s, cnt); s += " y encontre "; W3dEscribirInt(s, leidos);
            W3dmAviso(info, lineaCab, s);
        }
        if (cab.nombre == "UV")  capasUV.push_back(capa);
        if (cab.nombre == "COL") capasCol.push_back(capa);
        if (cab.nombre == "VG")  vgs.push_back(grupo);
        if (cab.nombre == "UVG") uvgs.push_back(grupo);
        if (cab.nombre == "SHAPE") formas.push_back(grupo);
    }

    // ---- "requiere": si nombra un bloque que no conocemos, el archivo se abre
    //      pero NO se puede pisar (degradar en silencio seria peor).
    for (size_t i = 0; i < requiere.size(); i++)
        if (!W3dMallaBloqueConocido(requiere[i])) {
            if (info) info->soloLectura = true;
            W3dmAviso(info, 1, "el archivo REQUIERE el bloque '" + requiere[i] +
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
    if (!vioV || !vioF) {
        std::string s = "el archivo no trae el bloque ";
        if (!vioV && !vioF) s += "V ni el bloque F";
        else if (!vioV)     s += "V";
        else                s += "F";
        s += " (esta cortado o no es una malla): NO toco la malla que ya estaba";
        W3dmAviso(info, lin.Nro(), s);
        if (info) {
            info->truncado = true;
            if (info->avisosDeMas > 0) {
                std::string t = "(y "; W3dEscribirInt(t, info->avisosDeMas);
                t += " aviso(s) mas que no muestro)";
                info->avisos.push_back(t);
            }
        }
        return false;
    }

    // =======================================================================
    //  RECONSTRUCCION
    // =======================================================================
    const int nP = (int)(pts.size() / 3);
    const int nF = (int)carasLados.size();
    int nC = 0;
    for (int f = 0; f < nF; f++) nC += carasLados[(size_t)f];

    // validacion de indices: fuera de rango = se descarta la CARA y se avisa.
    //
    //  LAS CAPAS SE RECORTAN, NO SE TIRAN. Antes UN SOLO indice mal tipeado en UNA
    //  linea F borraba TODAS las capas por esquina de la malla (capasUV.clear() +
    //  capasCol.clear() + nrm + uvrest): en un formato que se vende como editable a
    //  mano, un typo no puede costar todo el trabajo de texturizado. Y ademas el
    //  costo era invisible: sin capas UV el escritor emitia despues una "UVMap"
    //  DERIVADA llena de ceros, que PARECE dato y no lo es.
    //  Aca ya sabemos EXACTAMENTE que esquinas se fueron (las de las caras
    //  descartadas), asi que se arma la mascara de esquinas SOBREVIVIENTES y se
    //  recorta cada capa con ella. Lo unico que se pierde es lo que estaba en las
    //  caras rotas, que es lo que de verdad se perdio.
    {
        size_t off = 0;
        std::vector<int> lados2, idx2;
        lados2.reserve((size_t)nF);
        idx2.reserve(carasIdx.size());
        int malas = 0;
        for (int f = 0; f < nF; f++) {
            int L = carasLados[(size_t)f];
            bool ok = true;
            for (int i = 0; i < L; i++) {
                int v = carasIdx[off + (size_t)i];
                if (v < 0 || v >= nP) { ok = false; break; }
            }
            if (ok) {
                lados2.push_back(L);
                for (int i = 0; i < L; i++) idx2.push_back(carasIdx[off + (size_t)i]);
            } else malas++;
            off += (size_t)L;
        }
        if (malas > 0) {
            // la mascara de esquinas sobrevivientes se arma RECIEN ACA (segunda
            // pasada): en el camino normal -sin caras rotas- no se reserva ni un
            // byte de mas, que en el N95 es la diferencia entre cargar y fragmentar.
            std::vector<int> survC;
            survC.reserve((size_t)nC);
            size_t o2 = 0; int L0 = 0;
            for (int f = 0; f < nF; f++) {
                int L = carasLados[(size_t)f];
                bool ok = true;
                for (int i = 0; i < L; i++) {
                    int v = carasIdx[o2 + (size_t)i];
                    if (v < 0 || v >= nP) { ok = false; break; }
                }
                if (ok) for (int i = 0; i < L; i++) survC.push_back(L0 + i);
                o2 += (size_t)L;
                L0 += L;
            }
            std::string s = "descarte "; W3dEscribirInt(s, malas);
            s += " cara(s) con indices de punto fuera de rango (recorto las capas por esquina a las caras que quedaron)";
            W3dmAviso(info, 1, s);
            const int nCviejo = nC;
            for (size_t l = 0; l < capasUV.size(); l++) {
                if ((int)capasUV[l].uv.size() != nCviejo * 2) continue;  // ya no cuadraba: la agarra el chequeo de abajo
                std::vector<float> nu; nu.reserve(survC.size() * 2);
                for (size_t i = 0; i < survC.size(); i++) {
                    nu.push_back(capasUV[l].uv[(size_t)survC[i] * 2]);
                    nu.push_back(capasUV[l].uv[(size_t)survC[i] * 2 + 1]);
                }
                capasUV[l].uv.swap(nu);
            }
            for (size_t l = 0; l < capasCol.size(); l++) {
                if ((int)capasCol[l].col.size() != nCviejo * 4) continue;
                std::vector<unsigned char> nc; nc.reserve(survC.size() * 4);
                for (size_t i = 0; i < survC.size(); i++)
                    for (int q = 0; q < 4; q++) nc.push_back(capasCol[l].col[(size_t)survC[i] * 4 + (size_t)q]);
                capasCol[l].col.swap(nc);
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
                std::vector<int> viejoANuevo((size_t)(nCviejo > 0 ? nCviejo : 1), -1);
                for (size_t i = 0; i < survC.size(); i++) viejoANuevo[(size_t)survC[i]] = (int)i;
                for (size_t g = 0; g < uvgs.size(); g++) {
                    std::vector<int> ni; std::vector<float> np;
                    for (size_t k = 0; k < uvgs[g].idx.size() && k < uvgs[g].peso.size(); k++) {
                        int L = uvgs[g].idx[k];
                        if (L < 0 || L >= nCviejo || viejoANuevo[(size_t)L] < 0) continue;
                        ni.push_back(viejoANuevo[(size_t)L]); np.push_back(uvgs[g].peso[k]);
                    }
                    uvgs[g].idx.swap(ni); uvgs[g].peso.swap(np);
                }
            }
            carasLados.swap(lados2); carasIdx.swap(idx2);
            nC = 0; for (size_t f = 0; f < carasLados.size(); f++) nC += carasLados[f];
        }
    }
    const int nFok = (int)carasLados.size();

    // capas por esquina que no midan lo que tienen que medir: se descartan
    for (size_t i = 0; i < capasUV.size(); ) {
        if ((int)capasUV[i].uv.size() != nC * 2) {
            W3dmAviso(info, 1, "la capa UV '" + capasUV[i].nombre + "' no cubre todas las esquinas: la descarto");
            capasUV.erase(capasUV.begin() + (long)i);
        } else i++;
    }
    for (size_t i = 0; i < capasCol.size(); ) {
        if ((int)capasCol[i].col.size() != nC * 4) {
            W3dmAviso(info, 1, "la capa de color '" + capasCol[i].nombre + "' no cubre todas las esquinas: la descarto");
            capasCol.erase(capasCol.begin() + (long)i);
        } else i++;
    }
    if (!nrm.empty() && (int)nrm.size() != nC * 3) { W3dmAviso(info, 1, "NRM no cubre todas las esquinas: recalculo las normales"); nrm.clear(); }
    if (!uvrest.empty() && (int)uvrest.size() != nC * 2) { W3dmAviso(info, 1, "UVREST no cubre todas las esquinas: lo descarto"); uvrest.clear(); }

    if (uvActiva >= (int)capasUV.size())  uvActiva  = capasUV.empty()  ? -1 : 0;
    if (colActiva >= (int)capasCol.size()) colActiva = capasCol.empty() ? -1 : 0;
    if (uvActiva < -1)  uvActiva = -1;
    if (colActiva < -1) colActiva = -1;

    const CapaCorner* uvAct  = (uvActiva  >= 0) ? &capasUV[(size_t)uvActiva]   : 0;
    const CapaCorner* colAct = (colActiva >= 0 && !capasCol[(size_t)colActiva].porVertice)
                               ? &capasCol[(size_t)colActiva] : 0;

    // ---- MERGE DE RENDER ------------------------------------------------
    //  MISMA clave de 40 bytes y MISMO orden de primera aparicion que
    //  GenerarRender (main/edit/MeshEdit.cpp): pos(12) + uv activa(8) +
    //  normal por esquina(3) + color activo(4). Por eso el array de render se
    //  reproduce BIT A BIT sin haberlo guardado.
    std::vector<float>         vp;
    std::vector<signed char>   vn;
    std::vector<float>         vu;
    std::vector<unsigned char> vc;
    std::vector<int> puntoARender((size_t)(nP > 0 ? nP : 1), -1);
    // render-vert -> punto. Espejo del de arriba, pero COMPLETO: puntoARender se
    // queda con el PRIMER render-vert de cada punto, y una posicion puede tener
    // varios render-verts (una costura de UV o de color parte el vertice: son
    // unicos por posicion+color+UV y no se pueden fusionar sin romper el
    // sombreado ni las UV). Los pesos de los vertex groups se guardan POR
    // POSICION, asi que al cargar hay que repartirlos a TODOS.
    std::vector<int> renderAPunto;
    std::vector<MeshFace> faces;
    faces.reserve((size_t)nFok);
    vp.reserve((size_t)nC * 3); vn.reserve((size_t)nC * 3);
    vu.reserve((size_t)nC * 2); vc.reserve((size_t)nC * 4);

    // RVMAP valido? Si lo esta, MANDA el (ancla los keyframes de las vertex
    // anims). Si no cierra se ignora, se rehace el merge y se avisa: un RVMAP
    // mentiroso degrada a lento, nunca a roto.
    bool usarRvmap = false;
    int  rvN = 0;
    if ((int)rvmap.size() == nC && nC > 0) {
        int mx = -1; bool ok = true;
        for (int i = 0; i < nC && ok; i++) { if (rvmap[(size_t)i] < 0) ok = false; else if (rvmap[(size_t)i] > mx) mx = rvmap[(size_t)i]; }
        if (ok && mx >= 0 && mx < nC + 8) {
            std::vector<char> visto((size_t)mx + 1, 0);
            for (int i = 0; i < nC; i++) visto[(size_t)rvmap[(size_t)i]] = 1;
            for (int i = 0; i <= mx && ok; i++) if (!visto[(size_t)i]) ok = false; // tiene que cubrir 0..mx
            if (ok) { usarRvmap = true; rvN = mx + 1; }
        }
        if (!usarRvmap) W3dmAviso(info, 1, "RVMAP inconsistente: lo ignoro y rehago el merge de render");
    } else if (!rvmap.empty()) {
        W3dmAviso(info, 1, "RVMAP no cubre todas las esquinas: lo ignoro");
    }

    if (usarRvmap) {
        vp.assign((size_t)rvN * 3, 0.0f);
        vn.assign((size_t)rvN * 3, 0);
        vu.assign((size_t)rvN * 2, 0.0f);
        vc.assign((size_t)rvN * 4, 255);
        std::vector<char> puesto((size_t)rvN, 0);
        size_t off = 0; int L = 0;
        for (int f = 0; f < nFok; f++) {
            MeshFace mf;
            int lados = carasLados[(size_t)f];
            mf.idx.reserve((size_t)lados);
            for (int i = 0; i < lados; i++, L++) {
                int pi = carasIdx[off + (size_t)i];
                int gi = rvmap[(size_t)L];
                if (!puesto[(size_t)gi]) {
                    puesto[(size_t)gi] = 1;
                    vp[(size_t)gi * 3] = pts[(size_t)pi * 3]; vp[(size_t)gi * 3 + 1] = pts[(size_t)pi * 3 + 1]; vp[(size_t)gi * 3 + 2] = pts[(size_t)pi * 3 + 2];
                    if (!nrm.empty()) { vn[(size_t)gi * 3] = nrm[(size_t)L * 3]; vn[(size_t)gi * 3 + 1] = nrm[(size_t)L * 3 + 1]; vn[(size_t)gi * 3 + 2] = nrm[(size_t)L * 3 + 2]; }
                    else { vn[(size_t)gi * 3] = 0; vn[(size_t)gi * 3 + 1] = 127; vn[(size_t)gi * 3 + 2] = 0; }
                    if (uvAct) { vu[(size_t)gi * 2] = uvAct->uv[(size_t)L * 2]; vu[(size_t)gi * 2 + 1] = uvAct->uv[(size_t)L * 2 + 1]; }
                    if (colAct) for (int k = 0; k < 4; k++) vc[(size_t)gi * 4 + (size_t)k] = colAct->col[(size_t)L * 4 + (size_t)k];
                }
                if (puntoARender[(size_t)pi] < 0) puntoARender[(size_t)pi] = gi;
                if ((int)renderAPunto.size() <= gi) renderAPunto.resize((size_t)gi + 1, -1);
                renderAPunto[(size_t)gi] = pi;
                mf.idx.push_back(gi);
            }
            off += (size_t)lados;
            faces.push_back(mf);
        }
    } else {
        std::map<std::string, int> mapa;
        size_t off = 0; int L = 0;
        for (int f = 0; f < nFok; f++) {
            MeshFace mf;
            int lados = carasLados[(size_t)f];
            mf.idx.reserve((size_t)lados);
            for (int i = 0; i < lados; i++, L++) {
                int pi = carasIdx[off + (size_t)i];
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
                std::map<std::string, int>::iterator it = mapa.find(key);
                int gi;
                if (it != mapa.end()) gi = it->second;
                else {
                    gi = (int)(vp.size() / 3);
                    vp.push_back(px); vp.push_back(py); vp.push_back(pz);
                    vn.push_back(nbx); vn.push_back(nby); vn.push_back(nbz);
                    vu.push_back(u0); vu.push_back(v0);
                    vc.push_back(r); vc.push_back(g); vc.push_back(b); vc.push_back(a);
                    mapa[key] = gi;
                }
                if (puntoARender[(size_t)pi] < 0) puntoARender[(size_t)pi] = gi;
                if ((int)renderAPunto.size() <= gi) renderAPunto.resize((size_t)gi + 1, -1);
                renderAPunto[(size_t)gi] = pi;
                mf.idx.push_back(gi);
            }
            off += (size_t)lados;
            faces.push_back(mf);
        }
    }

    // ---- GEOMETRIA SUELTA: se anexa DESPUES de las caras y en el MISMO orden
    //      que usa GenerarRender (primero EDGE, despues POINT), asi la
    //      numeracion de los render-verts sueltos coincide.
    std::vector<int> nLoose, nLooseV;
    for (size_t i = 0; i + 1 < edgeSuelta.size(); i += 2) {
        int ab[2] = { edgeSuelta[i], edgeSuelta[i + 1] }, nn[2] = { -1, -1 };
        for (int s = 0; s < 2; s++) {
            int p = ab[s];
            if (p < 0 || p >= nP) { nn[s] = -1; continue; }
            if (puntoARender[(size_t)p] >= 0) { nn[s] = puntoARender[(size_t)p]; continue; }
            int gi = (int)(vp.size() / 3);
            vp.push_back(pts[(size_t)p * 3]); vp.push_back(pts[(size_t)p * 3 + 1]); vp.push_back(pts[(size_t)p * 3 + 2]);
            vn.push_back(0); vn.push_back(127); vn.push_back(0);
            vu.push_back(0.0f); vu.push_back(0.0f);
            vc.push_back(255); vc.push_back(255); vc.push_back(255); vc.push_back(255);
            puntoARender[(size_t)p] = gi; nn[s] = gi;
            if ((int)renderAPunto.size() <= gi) renderAPunto.resize((size_t)gi + 1, -1);
            renderAPunto[(size_t)gi] = p;
        }
        if (nn[0] >= 0 && nn[1] >= 0) { nLoose.push_back(nn[0]); nLoose.push_back(nn[1]); }
        else W3dmAviso(info, 1, "arista suelta con un punto fuera de rango: la descarto");
    }
    for (size_t i = 0; i < puntoSuelto.size(); i++) {
        int p = puntoSuelto[i];
        if (p < 0 || p >= nP) { W3dmAviso(info, 1, "vertice suelto fuera de rango: lo descarto"); continue; }
        // ya lo usa una cara/arista (o OTRO vertice suelto): no es un vertice aparte.
        // El escritor de Whisk3D nunca emite esto (a cada vertice suelto le da su propio
        // punto justamente para no soldarlos), asi que si aparece es de un archivo
        // editado a mano o de otro programa. Se fusiona, pero NO en silencio.
        if (puntoARender[(size_t)p] >= 0) {
            W3dmAviso(info, 1, "un vertice suelto cae en un punto que ya esta en uso: lo fusiono con el que ya estaba");
            continue;
        }
        int gi = (int)(vp.size() / 3);
        vp.push_back(pts[(size_t)p * 3]); vp.push_back(pts[(size_t)p * 3 + 1]); vp.push_back(pts[(size_t)p * 3 + 2]);
        vn.push_back(0); vn.push_back(127); vn.push_back(0);
        vu.push_back(0.0f); vu.push_back(0.0f);
        vc.push_back(255); vc.push_back(255); vc.push_back(255); vc.push_back(255);
        puntoARender[(size_t)p] = gi; nLooseV.push_back(gi);
        if ((int)renderAPunto.size() <= gi) renderAPunto.resize((size_t)gi + 1, -1);
        renderAPunto[(size_t)gi] = p;
    }

    const int nRV = (int)(vp.size() / 3);
    if (info) { info->rverts = declRverts; info->rvertsReales = nRV; }
    if (declRverts >= 0 && declRverts != nRV) {
        std::string s = "los vertices de render no dieron lo declarado (";
        W3dEscribirInt(s, declRverts); s += " contra "; W3dEscribirInt(s, nRV);
        s += "): las vertex anims van a necesitar el remapeo por posicion";
        W3dmAviso(info, 1, s);
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
    // cuadro base con W3dReposoVertexAnim), asi que lo que se acaba de leer ES el modelo. El
    // estado viaja con las posiciones tambien por esta puerta: se escribe explicito y no se
    // hereda lo que hubiera en la malla destino. Ver Mesh::posadaPorAnim.
    m->posadaPorAnim = false;
    if (nRV > 0) {
        m->vertex      = new GLfloat[(size_t)nRV * 3];
        m->normals     = new GLbyte[(size_t)nRV * 3];
        m->uv          = new GLfloat[(size_t)nRV * 2];
        m->vertexColor = new GLubyte[(size_t)nRV * 4];
        for (int i = 0; i < nRV * 3; i++) { m->vertex[i] = vp[(size_t)i]; m->normals[i] = (GLbyte)vn[(size_t)i]; }
        for (int i = 0; i < nRV * 2; i++) m->uv[i] = vu[(size_t)i];
        for (int i = 0; i < nRV * 4; i++) m->vertexColor[i] = (GLubyte)vc[(size_t)i];
    }

    if (!nombre.empty()) m->name = nombre;
    if (haySuave) m->meshSmooth = (suave != 0);
    if (hayPrim) {
        m->meshTipo = primTipo; m->meshSize = primS; m->meshSize2 = primS2;
        m->meshDepth = primD;   m->meshVerts = primV; m->meshVerts2 = primV2;
    } else m->meshTipo = -1;

    // caras + su mesh part y su shading por cara
    for (size_t i = 0; i < fpartF.size(); i++) {
        int f = fpartF[i];
        if (f >= 0 && f < (int)faces.size()) faces[(size_t)f].mat = fpartV[i];
        else W3dmAviso(info, 1, "FPART apunta a una cara que no existe: lo descarto");
    }
    for (size_t i = 0; i < fshadeF.size(); i++) {
        int f = fshadeF[i];
        if (f >= 0 && f < (int)faces.size()) faces[(size_t)f].smooth = (fshadeV[i] != 0) ? 1 : 0;
        else W3dmAviso(info, 1, "FSHADE apunta a una cara que no existe: lo descarto");
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
    W3dmLiberarCapas(m);
    for (size_t l = 0; l < capasUV.size(); l++) {
        UVMap* u = new UVMap(capasUV[l].nombre);
        u->uv.assign(capasUV[l].uv.begin(), capasUV[l].uv.end());
        m->uvMaps.push_back(u);
    }
    m->uvMapActivo = uvActiva;
    for (size_t l = 0; l < capasCol.size(); l++) {
        ColorLayer* c = new ColorLayer(capasCol[l].nombre);
        c->porVertice = capasCol[l].porVertice;
        c->color.assign(capasCol[l].col.begin(), capasCol[l].col.end());
        m->colorLayers.push_back(c);
    }
    m->colorActivo = colActiva;

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
    //  las UV). Antes el peso se le daba SOLO al representante -- puntoARender[p],
    //  el primer render-vert de esa posicion -- y los demas quedaban sin peso: en
    //  una malla grande con costuras se perdian cientos de pesos y el grupo
    //  quedaba incompleto (una animacion por grupos partia la malla en dos por la
    //  costura). Ahora el peso de una posicion va a TODOS sus render-verts.
    std::vector<std::vector<int> > rvsDePunto;
    if ((!vgs.empty() || !formas.empty()) && nP > 0) {
        rvsDePunto.resize((size_t)nP);
        for (int gi = 0; gi < nRV && gi < (int)renderAPunto.size(); gi++) {
            int p = renderAPunto[(size_t)gi];
            if (p >= 0 && p < nP) rvsDePunto[(size_t)p].push_back(gi);
        }
    }
    for (size_t g = 0; g < vgs.size(); g++) {
        VertexGroup* vg = new VertexGroup(vgs[g].nombre);
        int repartidos = 0;
        for (size_t k = 0; k < vgs[g].idx.size(); k++) {
            int p = vgs[g].idx[k];
            if (p < 0 || p >= nP || puntoARender[(size_t)p] < 0) { W3dmAviso(info, 1, "peso de vertex group con punto fuera de rango: lo descarto"); continue; }
            const float w = vgs[g].peso[k];
            const std::vector<int>* rvs = rvsDePunto.empty() ? NULL : &rvsDePunto[(size_t)p];
            if (rvs && !rvs->empty()) {
                for (size_t i = 0; i < rvs->size(); i++) {
                    vg->verts.push_back((*rvs)[i]);
                    vg->pesos.push_back(w);
                }
                if (rvs->size() > 1) repartidos += (int)rvs->size() - 1;
            } else {
                vg->verts.push_back(puntoARender[(size_t)p]);
                vg->pesos.push_back(w);
            }
        }
        (void)repartidos;   // el reparto es la conducta correcta: no se avisa
        m->vertexGroups.push_back(vg);
    }
    // SHAPE KEYS: de PUNTO a TODOS sus render-verts (el mismo espacio que los vertex groups de arriba, que es el
    // que usa el skinning por control-point)
    for (size_t g = 0; g < formas.size(); g++) {
        W3dShapeKey sk; sk.nombre = formas[g].nombre;
        for (size_t k = 0; k < formas[g].idx.size() && k * 3 + 2 < formas[g].d.size(); k++) {
            int p = formas[g].idx[k];
            if (p < 0 || p >= nP || puntoARender[(size_t)p] < 0) continue;
            const std::vector<int>* rvs = rvsDePunto.empty() ? NULL : &rvsDePunto[(size_t)p];
            if (rvs && !rvs->empty()) {
                for (size_t i = 0; i < rvs->size(); i++) {
                    sk.idx.push_back((*rvs)[i]);
                    sk.d.push_back(formas[g].d[k*3]); sk.d.push_back(formas[g].d[k*3+1]); sk.d.push_back(formas[g].d[k*3+2]);
                }
            } else {
                sk.idx.push_back(puntoARender[(size_t)p]);
                sk.d.push_back(formas[g].d[k*3]); sk.d.push_back(formas[g].d[k*3+1]); sk.d.push_back(formas[g].d[k*3+2]);
            }
        }
        m->shapeKeys.push_back(sk);
    }
    m->shapePesos.assign(m->shapeKeys.size(), 0.0f);
    // -1 ("ninguno activo") ES UN VALOR VALIDO y hay que respetarlo: normalizarlo a 0 hacia que
    // guardar -> abrir -> guardar diera bytes distintos por un dato que nadie cambio.
    m->grupoActivo = (vgActiva >= -1 && vgActiva < (int)m->vertexGroups.size()) ? vgActiva : (m->vertexGroups.empty() ? -1 : 0);

    // esquina -> render-vert, plano (se arma UNA vez: sin esto la busqueda seria
    // cuadratica y en una malla grande el N95 se queda pensando)
    std::vector<int> esquinaARender;
    esquinaARender.reserve((size_t)nC);
    for (size_t f = 0; f < m->faces3d.size(); f++)
        for (size_t c = 0; c < m->faces3d[f].idx.size(); c++)
            esquinaARender.push_back(m->faces3d[f].idx[c]);

    // UV groups: de ESQUINA a RENDER-VERT
    for (size_t g = 0; g < uvgs.size(); g++) {
        UVGroup* ug = new UVGroup(uvgs[g].nombre);
        std::vector<float> w((size_t)(nRV > 0 ? nRV : 1), 0.0f);
        for (size_t k = 0; k < uvgs[g].idx.size(); k++) {
            int L = uvgs[g].idx[k];
            if (L < 0 || L >= (int)esquinaARender.size()) { W3dmAviso(info, 1, "peso de UV group con esquina fuera de rango: lo descarto"); continue; }
            int gv = esquinaARender[(size_t)L];
            if (gv >= 0 && gv < nRV) w[(size_t)gv] = uvgs[g].peso[k];
        }
        for (int i = 0; i < nRV; i++) if (w[(size_t)i] > 0.0f) { ug->verts.push_back(i); ug->pesos.push_back(w[(size_t)i]); }
        m->uvGroups.push_back(ug);
    }
    m->uvGrupoActivo = (uvgActiva >= -1 && uvgActiva < (int)m->uvGroups.size()) ? uvgActiva : (m->uvGroups.empty() ? -1 : 0);

    // mesh parts (el material lo resuelve POR NOMBRE el que llama)
    m->materialsGroup.clear();
    if (info) info->materiales.clear();
    for (size_t i = 0; i < parts.size(); i++) {
        MaterialGroup mg;
        mg.name = parts[i];
        m->materialsGroup.push_back(mg);
        if (info) info->materiales.push_back(std::string());
    }
    if (m->materialsGroup.empty()) { m->materialsGroup.push_back(MaterialGroup()); if (info) info->materiales.push_back(std::string()); }
    for (size_t i = 0; i < partmatI.size(); i++) {
        int p = partmatI[i];
        if (p >= 0 && p < (int)m->materialsGroup.size()) { if (info) info->materiales[(size_t)p] = partmatN[i]; }
        else W3dmAviso(info, 1, "PARTMAT apunta a un mesh part que no existe: lo descarto");
    }
    // una cara que apunte a un mesh part inexistente cae al 0 (nunca se dibuja fuera de rango)
    for (size_t f = 0; f < m->faces3d.size(); f++)
        if (m->faces3d[f].mat < 0 || m->faces3d[f].mat >= (int)m->materialsGroup.size()) m->faces3d[f].mat = 0;

    // SHARP / SEAM: par de PUNTOS -> clave de POSICION (la misma que
    // Mesh::SharpEdgeKey), rearmada desde las posiciones ya leidas.
    m->sharpEdges.clear();
    m->seamEdges.clear();
    for (int pasada = 0; pasada < 2; pasada++) {
        const std::vector<int>& src = pasada ? seam : sharp;
        std::set<std::string>& dst = pasada ? m->seamEdges : m->sharpEdges;
        for (size_t i = 0; i + 1 < src.size(); i += 2) {
            int a = src[i], b = src[i + 1];
            if (a < 0 || a >= nP || b < 0 || b >= nP || a == b) { W3dmAviso(info, 1, "arista marcada con un punto fuera de rango: la descarto"); continue; }
            dst.insert(W3dmAristaKey(&pts[(size_t)a * 3], &pts[(size_t)b * 3]));
        }
    }

    // ---- SELLO + BLOQUES AJENOS ----------------------------------------
    m->w3dmAjenos.Limpiar();
    m->w3dmAjenos.bloques = ajenos;
    // la directiva "requiere" VIAJA CON LA MALLA: el escritor la re-emite tal cual.
    // Sin esto se auto-borraba en el primer guardado y el archivo se quedaba sin la
    // valvula que protege a los bloques que esta version no entiende.
    m->w3dmAjenos.requiere  = requiere;
    m->w3dmAjenos.truncado  = (info ? info->truncado : false);
    m->w3dmAjenos.nPuntos   = nP;
    m->w3dmAjenos.nEsquinas = nC;
    m->w3dmAjenos.nCaras    = (int)m->faces3d.size();
    {
        std::set<std::pair<int, int> > ar;
        size_t off = 0;
        for (size_t f = 0; f < carasLados.size(); f++) {
            int L = carasLados[f];
            for (int i = 0; i < L; i++) {
                int a = carasIdx[off + (size_t)i], b = carasIdx[off + (size_t)((i + 1) % L)];
                if (a != b) ar.insert(a < b ? std::make_pair(a, b) : std::make_pair(b, a));
            }
            off += (size_t)L;
        }
        for (size_t i = 0; i + 1 < edgeSuelta.size(); i += 2) {
            int a = edgeSuelta[i], b = edgeSuelta[i + 1];
            if (a >= 0 && a < nP && b >= 0 && b < nP && a != b)
                ar.insert(a < b ? std::make_pair(a, b) : std::make_pair(b, a));
        }
        m->w3dmAjenos.nAristas = (int)ar.size();
    }

    // LOS PUNTOS DEL ARCHIVO SON LA IDENTIDAD DE LOS VERTICES: render-vert -> el primer render-vert de SU
    // punto, en el mapa del skinning (vertCtrlPoint) y en el agrupamiento del editor (posRep). Antes el mapa
    // se rehacia POR POSICION al cargar y dos puntos distintos parados en el mismo lugar (cada uno con su
    // hueso) terminaban usando los pesos del primero: la malla se estiraba al animar. El que carga tiene que
    // conservar este posRep (CalcularBordes(..., reagruparPosRep = false)).
    m->vertCtrlPoint.assign((size_t)nRV, 0);
    m->posRep.assign((size_t)nRV, 0);
    for (int gi = 0; gi < nRV; gi++) {
        int p = (gi < (int)renderAPunto.size()) ? renderAPunto[(size_t)gi] : -1;
        int r = (p >= 0 && p < nP && puntoARender[(size_t)p] >= 0) ? puntoARender[(size_t)p] : gi;
        if (r > gi) r = gi;          // el representante es el MENOR indice (contrato de posRep)
        m->vertCtrlPoint[(size_t)gi] = r;
        m->posRep[(size_t)gi] = r;
    }
    m->uvSelVert.clear();
    // el techo de avisos no puede esconder que hay mas (ver W3dMallaInfo::avisos)
    if (info && info->avisosDeMas > 0) {
        std::string t = "(y "; W3dEscribirInt(t, info->avisosDeMas);
        t += " aviso(s) mas que no muestro)";
        info->avisos.push_back(t);
    }
    return true;
}
