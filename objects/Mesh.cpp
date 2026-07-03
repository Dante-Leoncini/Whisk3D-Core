#include "Mesh.h"
#include "w3dGraphics.h"    // abstraccion de graficos del engine (sin GL)
#include "CameraBase.h"     // g_renderCamPos (camara del render, para el chrome equirect)
#include "RenderColors.h"   // paleta de render del CORE (sin depender de la UI)
#include "render/OpcionesRender.h" // RenderType (view) + MaterialPreviewAmbient
#include <iostream>
#include <math.h> // C puro: compila en RVCT y PC por igual
#include <set>
#include <map>
#include <utility>
#include <string>
#include <cstring>

// NOTA IMPORTANTE: mucho de este codigo NO va aca y se va a borrar/simplificar.....
// es que hay codigo que va en el EDITOR 3d de Whisk3D y que no deberia estar en el codigo base del "core".
// esta parte necesita una reescritura... pero necesito pensarla mejor

// ===================================================
// Constructor
// ===================================================
Mesh::Mesh(Object* parent, Vector3 pos)
    : Object(parent, "Mesh", pos),
      vertexSize(0), vertex(NULL), vertexColor(NULL), normals(NULL),
      uv(NULL), facesSize(0), faces(NULL)
{
    //MUCHAS de estas definiciones se van a borrar. ya que NO son la base y son cosas mas relacionadas al editor 3d....
    //ejemplo: "edit" o "modificadorActivo" etc... eso es del Editor de Whisk3D

    meshTipo = -1;       // no es una primitiva regenerable por defecto
    meshSize = 2.0f;
    meshSize2 = 0.0f;
    meshDepth = 2.0f;
    meshVerts = 8;
    meshVerts2 = 8;
    meshSmooth = false; // flat por defecto (cada cara su normal)
    overlayLcache = -1.0f; // los buffers de normales se calculan al primer uso
    vertsAgrupados = 0;
    centroGeom = Vector3(0, 0, 0);
    radioGeom = 0.0f;
    edit = NULL; // la malla de edicion se crea on-demand al entrar a Edit Mode
    uvMapActivo = -1; colorActivo = -1; grupoActivo = -1; // sin capas hasta PoblarCapas
    modificadorActivo = -1; // stack de modificadores vacio (lo gestiona el editor)
    genVertex = NULL; genNormals = NULL; genUV = NULL; genColor = NULL; genFaces = NULL;
    genVertexSize = 0; genFacesSize = 0; genValido = false; // sin malla generada hasta que haya modificadores
    chromeExpPos = NULL; chromeExpUV = NULL; chromeExpCount = 0; chromeUVValid = false; chromeCacheEq = true; // reflejo (lazy)
    tangents = NULL; nmColors = NULL; tangentsValid = false; // normal mapping (lazy)
}

// libera las capas persistentes (uv/color/groups). Lo llaman el destructor y Regenerar.
void Mesh::LiberarCapas() {
    for (size_t i=0;i<uvMaps.size();i++)       delete uvMaps[i];
    for (size_t i=0;i<colorLayers.size();i++)  delete colorLayers[i];
    for (size_t i=0;i<vertexGroups.size();i++) delete vertexGroups[i];
    uvMaps.clear(); colorLayers.clear(); vertexGroups.clear();
    uvMapActivo = -1; colorActivo = -1; grupoActivo = -1;
    cornerNormal.clear(); // se rehace en PoblarCapas desde normals[]
}

// cantidad de CORNERS (esquinas de cara): a esto se indexan las capas por-corner.
int Mesh::ContarCorners() const {
    int n=0; for (size_t f=0;f<faces3d.size();f++) n += (int)faces3d[f].idx.size(); return n;
}

// crea las capas iniciales desde los arrays de render (uv[]/vertexColor[]) si no hay
// ninguna. Por CORNER (orden de faces3d): corner L=(cara f, esquina c) -> vert GPU
// faces3d[f].idx[c]. AUTO-HEAL: si la capa activa quedo de otro tamano (la geometria
// cambio en una edit-op), rehace las capas desde el render (la capa activa = lo que las
// ops preservaron en uv[]/vertexColor[], asi sus datos sobreviven). Idempotente si no.
void Mesh::PoblarCapas() {
    int nC = ContarCorners();
    if (nC <= 0) return;
    bool stale =
        (!uvMaps.empty() && uvMapActivo>=0 && uvMapActivo<(int)uvMaps.size() &&
         (int)uvMaps[uvMapActivo]->uv.size() != nC*2) ||
        (!colorLayers.empty() && colorActivo>=0 && colorActivo<(int)colorLayers.size() &&
         !colorLayers[colorActivo]->porVertice && (int)colorLayers[colorActivo]->color.size() != nC*4);
    if (stale) LiberarCapas(); // la geometria cambio -> rehacer (FASE 2b: remapear las NO-activas)
    if (uvMaps.empty() && uv) {
        UVMap* mp = new UVMap("UVMap"); mp->uv.resize((size_t)nC*2);
        int L=0; for (size_t f=0;f<faces3d.size();f++) for (size_t c=0;c<faces3d[f].idx.size();c++){
            int gv=faces3d[f].idx[c]; mp->uv[L*2]=uv[gv*2]; mp->uv[L*2+1]=uv[gv*2+1]; L++; }
        uvMaps.push_back(mp); uvMapActivo = 0;
    }
    if (colorLayers.empty() && vertexColor) {
        ColorLayer* cl = new ColorLayer("Col"); cl->color.resize((size_t)nC*4);
        int L=0; for (size_t f=0;f<faces3d.size();f++) for (size_t c=0;c<faces3d[f].idx.size();c++){
            int gv=faces3d[f].idx[c]; for(int q=0;q<4;q++) cl->color[L*4+q]=vertexColor[gv*4+q]; L++; }
        colorLayers.push_back(cl); colorActivo = 0;
    }
    // NORMAL autoritativa por corner: si no esta o quedo de otro tamaño (op que no la
    // acarreo) la rehago desde normals[]. Las ops que SI la acarrean dejan el size OK.
    if ((int)cornerNormal.size() != nC*3 && normals) {
        cornerNormal.resize((size_t)nC*3);
        int L=0; for (size_t f=0;f<faces3d.size();f++) for (size_t c=0;c<faces3d[f].idx.size();c++){
            int gv=faces3d[f].idx[c]; cornerNormal[L*3]=normals[gv*3]; cornerNormal[L*3+1]=normals[gv*3+1]; cornerNormal[L*3+2]=normals[gv*3+2]; L++; }
    }
}

// EL RENDER SE DERIVA DE LA CAPA ACTIVA: copia la UVMap activa + la ColorLayer activa
// (por corner) a uv[]/vertexColor[] (por GPU vert). Lo llama el editor al cambiar de capa
// activa o al editar una capa. (Sin re-split de verts: las capas de PoblarCapas/duplicadas
// comparten el seam del render; cambiar seams es FASE 4 con GenerarRender.)
void Mesh::AplicarCapasAlRender() {
    int nC = ContarCorners();
    if (nC <= 0) return;
    UVMap* um = (uvMapActivo>=0 && uvMapActivo<(int)uvMaps.size()) ? uvMaps[uvMapActivo] : NULL;
    ColorLayer* cl = (colorActivo>=0 && colorActivo<(int)colorLayers.size()) ? colorLayers[colorActivo] : NULL;
    if (um && (int)um->uv.size() != nC*2) um = NULL;       // guard de tamano
    if (cl && (int)cl->color.size() != nC*4) cl = NULL;    // la capa SIEMPRE guarda por-corner (nC*4)
    bool tCN = (normals && (int)cornerNormal.size() == nC*3); // normal autoritativa -> render
    if (!um && !cl && !tCN) return;
    int L = 0;
    for (size_t f=0;f<faces3d.size();f++) for (size_t c=0;c<faces3d[f].idx.size();c++) {
        int gv = faces3d[f].idx[c];
        if (um && uv)          { uv[gv*2]=um->uv[L*2]; uv[gv*2+1]=um->uv[L*2+1]; }
        if (cl && vertexColor) { for (int q=0;q<4;q++) vertexColor[gv*4+q]=cl->color[L*4+q]; }
        if (tCN)               { normals[gv*3]=cornerNormal[L*3]; normals[gv*3+1]=cornerNormal[L*3+1]; normals[gv*3+2]=cornerNormal[L*3+2]; }
        L++;
    }
    // capa por-VERTICE: el color se COLAPSA por grupo de posicion (todos los verts coincidentes
    // toman el color del primero) -> 1 color por vertice. (La capa sigue guardando por-corner: el
    // toggle es no-destructivo, volver a por-corner re-bakea.) El export auto-detecta per-vertice.
    if (cl && cl->porVertice && vertexColor) {
        std::map<std::string,int> rep;
        for (int i = 0; i < vertexSize; i++) {
            std::string k((const char*)&vertex[i*3], 12);
            std::map<std::string,int>::iterator it = rep.find(k);
            if (it == rep.end()) rep[k] = i;
            else { int r = it->second; for (int q = 0; q < 4; q++) vertexColor[i*4+q] = vertexColor[r*4+q]; }
        }
    }
}

// duplican la capa ACTIVA y dejan la copia como activa (boton "+" de la pestaña Vertices).
// ===== Las DOS unicas puertas al render (abstraccion: las ops NO tocan vertex[]/faces3d a
//       mano -> integridad). RefrescarRender = edicion IN-PLACE rapida (no cambia topologia);
//       GenerarRender = REBUILD completo (cambio de topologia). =====


// REBUILD COMPLETO del render: DESTRUYE los arrays y los rehace desde los CORNERS (faces3d)
// + las CAPAS ACTIVAS. Expande cada corner a un vertice GPU MERGEANDO los identicos (pos +
// uv + normal + color) para que las mallas smooth NO se inflen. Es la UNICA fuente de verdad
// del render ante cambios de TOPOLOGIA: las edit-ops construyen faces3d + las capas y llaman
// aca (asi TODAS las capas sobreviven, sin meter interpolacion en cada op). LENTO -> usar
// solo cuando cambia la topologia; para mover/pintar usar RefrescarRender (in-place).
void Mesh::GenerarRender() {
    int nC = ContarCorners();
    if (nC <= 0 || !vertex) return;
    chromeUVValid = false; // la geometria cambia -> recalcular el reflejo equirect
    tangentsValid = false; // ... y las tangentes del normal map (dependen de pos+UV)
    // FLAT (cubo/plano): la normal autoritativa por corner = la de su CARA. Asi el merge de
    // abajo (la clave incluye la normal) NO une verts de caras distintas -> shading plano tras
    // extrude/loop cut. SMOOTH: normal por GRUPO de suavizado (promedia las caras de alrededor;
    // un borde sharp corta el grupo). Sin bordes sharp = todo suave; con sharp = cilindro.
    // Path rapido PorCara SOLO si es flat global y NINGUNA cara tiene override per-cara (Face>Shade sobre seleccion);
    // si hay override o es smooth -> ConSharp (respeta el flag smooth POR CARA de cada faces3d).
    bool hayOverride=false; for (size_t f=0;f<faces3d.size();f++) if (faces3d[f].smooth>=0){ hayOverride=true; break; }
    if (!meshSmooth && !hayOverride) CornerNormalPorCara();
    else                            CornerNormalConSharp();
    UVMap* um = (uvMapActivo>=0 && uvMapActivo<(int)uvMaps.size()) ? uvMaps[uvMapActivo] : NULL;
    ColorLayer* cl = (colorActivo>=0 && colorActivo<(int)colorLayers.size()) ? colorLayers[colorActivo] : NULL;
    bool tUV  = um && (int)um->uv.size()==nC*2;
    bool tCol = cl && !cl->porVertice && (int)cl->color.size()==nC*4;
    bool tNor = (normals != NULL);
    bool tCN  = ((int)cornerNormal.size() == nC*3); // normal AUTORITATIVA por corner

    std::map<std::string,int> mapa; // clave (pos+uv+normal+color) -> indice GPU nuevo (merge)
    std::vector<GLfloat> vp; std::vector<GLbyte> vn; std::vector<GLfloat> vu; std::vector<GLubyte> vc;
    std::vector<MeshFace> nf3d; nf3d.reserve(faces3d.size());
    std::vector<int> oldToNew(vertexSize, -1); // GPU viejo -> nuevo (1er corner) para remapear loose edges
    int L = 0;
    for (size_t f=0;f<faces3d.size();f++) { MeshFace mf; mf.mat = faces3d[f].mat; mf.smooth = faces3d[f].smooth; // conserva mesh part + shading por cara
        for (size_t c=0;c<faces3d[f].idx.size();c++) {
            int gv = faces3d[f].idx[c];
            float px=vertex[gv*3], py=vertex[gv*3+1], pz=vertex[gv*3+2];
            GLbyte nbx=0,nby=127,nbz=0; // normal del corner: autoritativa si esta, sino el render viejo
            if (tCN){ nbx=cornerNormal[L*3]; nby=cornerNormal[L*3+1]; nbz=cornerNormal[L*3+2]; }
            else if (tNor){ nbx=normals[gv*3]; nby=normals[gv*3+1]; nbz=normals[gv*3+2]; }
            float u0 = tUV ? um->uv[L*2] : 0.0f, v0 = tUV ? um->uv[L*2+1] : 0.0f;
            GLubyte r=255,g=255,b=255,a=255; if (tCol){ r=cl->color[L*4];g=cl->color[L*4+1];b=cl->color[L*4+2];a=cl->color[L*4+3]; }
            char buf[40]; int p=0;
            memcpy(buf+p,&px,4);p+=4; memcpy(buf+p,&py,4);p+=4; memcpy(buf+p,&pz,4);p+=4;
            memcpy(buf+p,&u0,4);p+=4; memcpy(buf+p,&v0,4);p+=4;
            buf[p++]=(char)nbx;buf[p++]=(char)nby;buf[p++]=(char)nbz;
            buf[p++]=(char)r;buf[p++]=(char)g;buf[p++]=(char)b;buf[p++]=(char)a;
            std::string key(buf,p);
            std::map<std::string,int>::iterator it=mapa.find(key);
            int gi;
            if (it!=mapa.end()) gi=it->second;
            else { gi=(int)(vp.size()/3);
                vp.push_back(px);vp.push_back(py);vp.push_back(pz);
                vn.push_back(nbx);vn.push_back(nby);vn.push_back(nbz);
                vu.push_back(u0);vu.push_back(v0);
                vc.push_back(r);vc.push_back(g);vc.push_back(b);vc.push_back(a);
                mapa[key]=gi; }
            mf.idx.push_back(gi); if (gv>=0 && gv<(int)oldToNew.size() && oldToNew[gv]<0) oldToNew[gv]=gi; L++;
        }
        nf3d.push_back(mf);
    }
    // LOOSE EDGES: remapear sus extremos al GPU nuevo (preservarlos en el rebuild). Un
    // extremo que no aparece en ninguna cara se agrega como vert nuevo (pos del vert viejo).
    std::vector<int> nLoose; nLoose.reserve(looseEdges.size());
    for (size_t i=0;i+1<looseEdges.size();i+=2){
        int ab[2]={looseEdges[i],looseEdges[i+1]}, nn[2];
        for (int s=0;s<2;s++){ int o=ab[s];
            if (o>=0 && o<(int)oldToNew.size() && oldToNew[o]>=0) nn[s]=oldToNew[o];
            else if (o>=0 && o<vertexSize){ int gi=(int)(vp.size()/3);
                vp.push_back(vertex[o*3]);vp.push_back(vertex[o*3+1]);vp.push_back(vertex[o*3+2]);
                vn.push_back(0);vn.push_back(127);vn.push_back(0);
                vu.push_back(0);vu.push_back(0);
                vc.push_back(255);vc.push_back(255);vc.push_back(255);vc.push_back(255);
                oldToNew[o]=gi; nn[s]=gi;
            } else nn[s]=-1; }
        if (nn[0]>=0 && nn[1]>=0){ nLoose.push_back(nn[0]); nLoose.push_back(nn[1]); }
    }
    int nuevoN=(int)(vp.size()/3); if (nuevoN<=0) return;
    delete[] vertex;      vertex=new GLfloat[nuevoN*3];  for(int i=0;i<nuevoN*3;i++) vertex[i]=vp[i];
    delete[] normals;     normals=new GLbyte[nuevoN*3];  for(int i=0;i<nuevoN*3;i++) normals[i]=vn[i];
    delete[] uv;          uv=new GLfloat[nuevoN*2];      for(int i=0;i<nuevoN*2;i++) uv[i]=vu[i];
    delete[] vertexColor; vertexColor=new GLubyte[nuevoN*4]; for(int i=0;i<nuevoN*4;i++) vertexColor[i]=vc[i];
    vertexSize=nuevoN;
    faces3d.swap(nf3d);   // misma cantidad/orden de corners -> las capas siguen validas
    looseEdges.swap(nLoose); // bordes sueltos remapeados a los GPU nuevos
    ReagruparMeshParts(); // arma el index buffer + los rangos por mesh part (mf.mat)
    GenerarMallaModificada(); // EDITOR: re-aplica el stack de modificadores -> malla generada (render). No-op sin stack
    CalcularBordes(); // recomputa posRep/edges/centroGeom + invalida el edit (geometria nueva)
}

// ============================================================================
//  OPTIMIZACION DE CACHE DE VERTICES — algoritmo de Tom Forsyth ("Linear-Speed
//  Vertex Cache Optimisation", 2006), que es PUBLICO y de uso libre. Esto es una
//  IMPLEMENTACION PROPIA de Whisk3D escrita desde la descripcion del algoritmo
//  (NO copia codigo del PowerVR SDK ni de ningun tercero) -> mantiene Whisk3D MIT.
//  Reordena los triangulos del index buffer para maximizar los aciertos del cache
//  post-transform del GPU (menos transformaciones de vertice repetidas). Ganancia
//  grande en GPUs tile-based como el PowerVR MBX del N95. NO toca el vertex buffer:
//  solo el ORDEN de los indices -> la malla se ve identica.
// ============================================================================
static const int   kVC_Size       = 32;    // tamano del cache que asumimos
static const float kVC_DecayPower  = 1.5f;
static const float kVC_LastTri     = 0.75f;
static const float kVC_ValScale    = 2.0f;
static const float kVC_ValPower    = 0.5f;

// score de un vertice: "que tan arriba esta en el cache" + bonus por "pocos triangulos
// pendientes lo usan" (conviene gastarlo ya). -1 = ningun triangulo pendiente lo usa.
static float VCacheScore(int trisPend, int posCache) {
    if (trisPend <= 0) return -1.0f;
    float score = 0.0f;
    if (posCache >= 0) {
        if (posCache < 3) score = kVC_LastTri;                 // los 3 verts del triangulo recien emitido
        else { float s = 1.0f - (float)(posCache - 3) / (float)(kVC_Size - 3); score = powf(s, kVC_DecayPower); }
    }
    score += kVC_ValScale * powf((float)trisPend, -kVC_ValPower);
    return score;
}

// reordena IN-PLACE los 3*numTris indices de 'idx' (una mesh part) para el cache de vertices.
static void OptimizarCacheVertices(MeshIndex* idx, int numTris, int numVerts) {
    if (!idx || numVerts <= 0 || numTris < 64) return; // mallas chicas entran enteras al cache: no hace falta

    // adyacencia vertice -> triangulos que lo usan (formato CSR)
    std::vector<int> cuenta(numVerts, 0);
    for (int i = 0; i < numTris*3; i++) { int v=(int)idx[i]; if (v>=0&&v<numVerts) cuenta[v]++; }
    std::vector<int> off(numVerts+1, 0);
    for (int v = 0; v < numVerts; v++) off[v+1] = off[v] + cuenta[v];
    std::vector<int> trisDe(off[numVerts]);
    { std::vector<int> cur(off.begin(), off.end()-1);
      for (int t=0;t<numTris;t++) for(int k=0;k<3;k++){ int v=(int)idx[t*3+k]; if (v>=0&&v<numVerts) trisDe[cur[v]++]=t; } }

    std::vector<int>   pend(cuenta);             // triangulos pendientes por vertice
    std::vector<int>   posCache(numVerts, -1);
    std::vector<float> scoreV(numVerts);
    for (int v=0;v<numVerts;v++) scoreV[v] = VCacheScore(pend[v], -1);
    std::vector<char>  usado(numTris, 0);
    std::vector<float> scoreT(numTris);
    for (int t=0;t<numTris;t++) scoreT[t] = scoreV[(int)idx[t*3]] + scoreV[(int)idx[t*3+1]] + scoreV[(int)idx[t*3+2]];

    std::vector<MeshIndex> salida; salida.reserve(numTris*3);
    std::vector<int> cache; cache.reserve(kVC_Size + 4); // LRU, MRU al frente

    int mejor=-1; float mejorS=-1.0f;
    for (int t=0;t<numTris;t++) if (scoreT[t] > mejorS){ mejorS=scoreT[t]; mejor=t; }

    for (int e=0; e<numTris; e++) {
        if (mejor < 0) { // fallback: el mejor entre TODOS los pendientes (arranca una componente nueva)
            mejorS=-1.0f; for (int t=0;t<numTris;t++) if (!usado[t] && scoreT[t]>mejorS){ mejorS=scoreT[t]; mejor=t; }
            if (mejor < 0) break;
        }
        int t = mejor; usado[t]=1;
        int v[3] = { (int)idx[t*3], (int)idx[t*3+1], (int)idx[t*3+2] };
        for (int k=0;k<3;k++) salida.push_back((MeshIndex)v[k]);
        for (int k=0;k<3;k++) pend[v[k]]--;
        // mover v[0..2] al frente del cache (sin duplicar)
        for (int k=0;k<3;k++){ int vk=v[k];
            for (size_t i=0;i<cache.size();i++) if (cache[i]==vk){ cache.erase(cache.begin()+i); break; }
            cache.insert(cache.begin(), vk); }
        // los que pasan kVC_Size salen del cache; junto los AFECTADOS (en cache + salidos) para re-scorear
        std::vector<int> afect;
        while ((int)cache.size() > kVC_Size){ int vo=cache.back(); cache.pop_back(); posCache[vo]=-1; afect.push_back(vo); }
        for (size_t i=0;i<cache.size();i++){ posCache[cache[i]]=(int)i; afect.push_back(cache[i]); }
        for (size_t a=0;a<afect.size();a++){ int vv=afect[a]; scoreV[vv]=VCacheScore(pend[vv], posCache[vv]); }
        // re-score de los triangulos de los afectados + trackeo el mejor entre los tocados
        mejor=-1; mejorS=-1.0f;
        for (size_t a=0;a<afect.size();a++){ int vv=afect[a];
            for (int j=off[vv]; j<off[vv+1]; j++){ int tt=trisDe[j]; if (usado[tt]) continue;
                scoreT[tt] = scoreV[(int)idx[tt*3]] + scoreV[(int)idx[tt*3+1]] + scoreV[(int)idx[tt*3+2]];
                if (scoreT[tt] > mejorS){ mejorS=scoreT[tt]; mejor=tt; } } }
    }
    for (int i=0;i<numTris*3 && i<(int)salida.size();i++) idx[i] = salida[i];
}

// Reconstruye el index buffer (faces[]) AGRUPANDO los triangulos por material POR-CARA (mf.mat), y
// los rangos de cada mesh part (materialsGroup[g].startDrawn/indicesDrawnCount). Antes GenerarRender
// colapsaba TODO a un grupo (perdia los mesh parts al editar). NO toca vertices/uv/normales/color ni
// el edit mesh: por eso Assign/Delete pueden usarla SIN un GenerarRender completo (la edicion sigue
// viva). Se preservan las entradas de materialsGroup (nombre+material); las vacias quedan con count 0.
void Mesh::ReagruparMeshParts() {
    int nGrupos = (int)materialsGroup.size();
    { int mx = 0; for (size_t f=0;f<faces3d.size();f++){ int m=faces3d[f].mat; if (m<0){ faces3d[f].mat=0; m=0; } if (m>mx) mx=m; }
      if (mx+1 > nGrupos) nGrupos = mx+1; }
    if (nGrupos < 1) nGrupos = 1;
    while ((int)materialsGroup.size() < nGrupos){ MaterialGroup g; materialsGroup.push_back(g); } // pad (nombre default)
    std::vector<MeshIndex> tris; // MeshIndex: en PC los indices pueden pasar 65535 (no truncar a 16 bits)
    for (int gi=0; gi<(int)materialsGroup.size(); gi++){
        materialsGroup[gi].startDrawn = (int)tris.size();
        for (size_t f=0;f<faces3d.size();f++){ if (faces3d[f].mat != gi) continue;
            const std::vector<int>& idx=faces3d[f].idx;
            for (size_t k=1;k+1<idx.size();k++){ tris.push_back((MeshIndex)idx[0]);tris.push_back((MeshIndex)idx[k]);tris.push_back((MeshIndex)idx[k+1]); } }
        materialsGroup[gi].indicesDrawnCount = (int)tris.size() - materialsGroup[gi].startDrawn;
    }
    facesSize=(int)tris.size(); delete[] faces; faces=new MeshIndex[facesSize>0?facesSize:1];
    for (int i=0;i<facesSize;i++) faces[i]=tris[i];
    OptimizarCacheRender(); // reordena los triangulos de cada mesh part para el cache de vertices (no cambia la geometria)
}

// optimiza el cache de vertices de CADA mesh part del render. Lo llaman ReagruparMeshParts (edits de malla) y el
// importador (tras ConvertToES1) -> el index buffer queda cache-friendly venga de donde venga.
void Mesh::OptimizarCacheRender() {
    if (!faces || facesSize < 192) return; // <64 triangulos: entran enteros al cache, no hace falta
    // por cada mesh part: medimos su ACMR (cache miss ratio, FIFO de 16). Si YA viene cache-coherente (orden
    // bueno, ej. un OBJ bien exportado) NO vale la pena el pase de Forsyth -> lo saltamos. Solo optimizamos las
    // que vienen con orden malo (procedurales, algunos exporters, resultado de editar). El optimo ronda ~0.83.
    for (size_t gi=0; gi<materialsGroup.size(); gi++) {
        int start = materialsGroup[gi].startDrawn, cnt = materialsGroup[gi].indicesDrawnCount;
        int gt = cnt/3;
        if (gt < 64 || start < 0 || start+cnt > facesSize) continue;
        float acmr; { std::vector<int> f; int m=0; for(int i=0;i<cnt;i++){ int v=(int)faces[start+i]; bool h=false; for(size_t j=0;j<f.size();j++) if(f[j]==v){h=true;break;} if(!h){ m++; f.insert(f.begin(),v); if((int)f.size()>16) f.pop_back(); } } acmr=(float)m/gt; }
        if (acmr > 1.0f) OptimizarCacheVertices(&faces[start], gt, vertexSize);
    }
}

// Mesh part NUEVO vacio (sin caras). Devuelve su indice. El render lo deja con count 0.
// ===================================================
// Destructor
// ===================================================
Mesh::~Mesh() {
    //LiberarMemoria();
    LiberarCapas();
    InvalidarEdit();
    LiberarModificadores(); // definida en el editor (como InvalidarEdit): libera el stack de modificadores
    LiberarMallaModificada(); // libera las gen buffers
}

// ===================================================
// Tipo de objeto
// ===================================================
ObjectType Mesh::getType() {
    return ObjectType::mesh;
}

// ===================================================
// Liberar memoria
// ===================================================
void Mesh::LiberarMemoria() {
    delete[] vertex;
    delete[] vertexColor;
    delete[] normals;
    delete[] uv;

    LiberarCapas(); // uv maps / color layers / vertex groups

    delete[] faces;
    materialsGroup.clear();
}

// ===================================================
// Renderizado
// ===================================================
// --- NORMAL MAPPING (DOT3) ---------------------------------------------------------------------------------
// Base tangente POR VERTICE de render (T en .xyz, handedness en .w). Acumula por triangulo desde pos+UV,
// ortonormaliza contra la normal y guarda. CACHE: solo recalcula si cambio la geometria/UV (tangentsValid).
void Mesh::CalcularTangentes() {
    if (tangentsValid && tangents) return;
    if (!vertex || !uv || !normals || !faces || vertexSize <= 0 || facesSize < 3) return;
    delete[] tangents; tangents = new GLfloat[vertexSize * 4];
    std::vector<Vector3> tanAcc(vertexSize, Vector3(0,0,0));
    std::vector<Vector3> bitAcc(vertexSize, Vector3(0,0,0));
    for (int i = 0; i + 2 < facesSize; i += 3) {
        int a = faces[i], b = faces[i+1], c = faces[i+2];
        Vector3 p0(vertex[a*3], vertex[a*3+1], vertex[a*3+2]);
        Vector3 p1(vertex[b*3], vertex[b*3+1], vertex[b*3+2]);
        Vector3 p2(vertex[c*3], vertex[c*3+1], vertex[c*3+2]);
        float u0=uv[a*2], v0=uv[a*2+1], u1=uv[b*2], v1=uv[b*2+1], u2=uv[c*2], v2=uv[c*2+1];
        Vector3 e1 = p1 - p0, e2 = p2 - p0;
        float du1=u1-u0, dv1=v1-v0, du2=u2-u0, dv2=v2-v0;
        float d = du1*dv2 - du2*dv1;
        if (d > -1e-9f && d < 1e-9f) continue;     // triangulo degenerado en UV
        float r = 1.0f / d;
        Vector3 T  = (e1*dv2 - e2*dv1) * r;
        Vector3 Bt = (e2*du1 - e1*du2) * r;
        tanAcc[a] += T; tanAcc[b] += T; tanAcc[c] += T;
        bitAcc[a] += Bt; bitAcc[b] += Bt; bitAcc[c] += Bt;
    }
    for (int v = 0; v < vertexSize; v++) {
        Vector3 N(normals[v*3]/127.0f, normals[v*3+1]/127.0f, normals[v*3+2]/127.0f); N = N.Normalized();
        Vector3 T = tanAcc[v];
        T = T - N * N.Dot(T);                       // Gram-Schmidt (T perpendicular a N)
        if (T.LengthSq() < 1e-12f) {                // sin UV utiles: cualquier perpendicular a N
            T = Vector3(1,0,0); if (N.Dot(T) > 0.9f || N.Dot(T) < -0.9f) T = Vector3(0,1,0);
            T = T - N * N.Dot(T);
        }
        T = T.Normalized();
        float hand = (Vector3::Cross(N, T).Dot(bitAcc[v]) < 0.0f) ? -1.0f : 1.0f;
        tangents[v*4] = T.x; tangents[v*4+1] = T.y; tangents[v*4+2] = T.z; tangents[v*4+3] = hand;
    }
    tangentsValid = true;
}

// L (vector a la luz) en TANGENT-SPACE por vertice -> nmColors (el "primary color" del DOT3). luzLocal = la luz
// en el espacio LOCAL de la malla. Se llama cada frame (la luz/camara se mueven). [-1,1] -> [0,1]*255.
void Mesh::ActualizarNormalMapColors(const Vector3& luzLocal) {
    if (!tangents || !normals || !vertex || vertexSize <= 0) return;
    if (!nmColors) nmColors = new GLubyte[vertexSize * 4];
    for (int v = 0; v < vertexSize; v++) {
        Vector3 N(normals[v*3]/127.0f, normals[v*3+1]/127.0f, normals[v*3+2]/127.0f); N = N.Normalized();
        Vector3 T(tangents[v*4], tangents[v*4+1], tangents[v*4+2]);
        Vector3 B = Vector3::Cross(N, T) * tangents[v*4+3];
        Vector3 P(vertex[v*3], vertex[v*3+1], vertex[v*3+2]);
        Vector3 L = (luzLocal - P).Normalized();
        float lt = L.Dot(T), lb = L.Dot(B), ln = L.Dot(N);
        nmColors[v*4]   = (GLubyte)((lt*0.5f+0.5f)*255.0f);
        nmColors[v*4+1] = (GLubyte)((lb*0.5f+0.5f)*255.0f);
        nmColors[v*4+2] = (GLubyte)((ln*0.5f+0.5f)*255.0f);
        nmColors[v*4+3] = 255;
    }
}

// CHROME EQUIRECTANGULAR 360 (calidad, para renders): calcula por SOFTWARE la UV del reflejo de cada
// vertice. Proyecta el vector de reflexion (vista->vertice reflejado en el normal, en MUNDO) a coords
// equirectangulares (longitud=atan2, latitud=acos). CACHE: solo recalcula si cambio la camara, la matriz de
// mundo o la geometria -> en una toma estatica cuesta CERO; solo "paga" al orbitar (clave para el N95).
void Mesh::ActualizarChromeUV(bool equirect) {
    if (vertexSize <= 0 || !vertex || !normals || facesSize <= 0 || !faces) return;
    Matrix4 W; GetWorldMatrix(W);
    Vector3 cam = g_renderCamPos;
    // base de camara (para el MATCAP, espacio de OJO): right/up/forward en mundo
    Vector3 cr = g_renderCamRight, cu = g_renderCamUp, cf = g_renderCamForward;
    // cache hit: mismo modo + misma camara + misma matriz de mundo + misma cantidad de corners -> nada que hacer
    if (chromeUVValid && chromeExpUV && chromeExpCount == facesSize && chromeCacheEq == equirect) {
        bool igual = (chromeCacheCam.x == cam.x && chromeCacheCam.y == cam.y && chromeCacheCam.z == cam.z);
        for (int i = 0; igual && i < 16; i++) if (chromeCacheW[i] != W.m[i]) igual = false;
        if (igual) return;
    }
    if (!chromeExpUV || chromeExpCount != facesSize) {
        delete[] chromeExpPos; chromeExpPos = new GLfloat[facesSize * 3];
        delete[] chromeExpUV;  chromeExpUV  = new GLfloat[facesSize * 2];
        chromeExpCount = facesSize;
    }
    const float PI = 3.14159265358979f;
    // POR TRIANGULO (3 corners): el EQUIRECT corrige costura/polo por-cara; el MATCAP no las tiene (disco).
    for (int t = 0; t + 2 < facesSize; t += 3) {
        float u[3], v[3]; bool polo[3]; Vector3 lp[3];
        for (int k = 0; k < 3; k++) {
            int vi = faces[t + k];
            lp[k] = Vector3(vertex[vi*3], vertex[vi*3+1], vertex[vi*3+2]);                 // pos local
            Vector3 ln(normals[vi*3]/127.0f, normals[vi*3+1]/127.0f, normals[vi*3+2]/127.0f);
            Vector3 wp = W * lp[k];
            Vector3 wn(W.m[0]*ln.x + W.m[4]*ln.y + W.m[8]*ln.z,
                       W.m[1]*ln.x + W.m[5]*ln.y + W.m[9]*ln.z,
                       W.m[2]*ln.x + W.m[6]*ln.y + W.m[10]*ln.z);
            wn = wn.Normalized();
            polo[k] = false;
            if (equirect) {
                // EQUIRECT 360: reflejo en MUNDO -> lat-long
                Vector3 I = (wp - cam).Normalized();
                float dd = I.x*wn.x + I.y*wn.y + I.z*wn.z;
                Vector3 R(I.x - 2*dd*wn.x, I.y - 2*dd*wn.y, I.z - 2*dd*wn.z);
                float ry = R.y < -1.0f ? -1.0f : (R.y > 1.0f ? 1.0f : R.y);
                u[k] = atan2f(R.z, R.x) / (2.0f*PI) + 0.5f;
                v[k] = acosf(ry) / PI;
                polo[k] = (ry > 0.995f || ry < -0.995f);
            } else {
                // MATCAP (sphere-map, replica de GL_SPHERE_MAP): en espacio de OJO. eye=R^T*(wp-cam), z hacia +.
                Vector3 rel = wp - cam;
                Vector3 ep(rel.x*cr.x + rel.y*cr.y + rel.z*cr.z,
                           rel.x*cu.x + rel.y*cu.y + rel.z*cu.z,
                         -(rel.x*cf.x + rel.y*cf.y + rel.z*cf.z));  // -forward = +Z del eye space
                Vector3 en(wn.x*cr.x + wn.y*cr.y + wn.z*cr.z,
                           wn.x*cu.x + wn.y*cu.y + wn.z*cu.z,
                         -(wn.x*cf.x + wn.y*cf.y + wn.z*cf.z));
                Vector3 I = ep.Normalized();                       // direccion de vista (ojo en el origen)
                float dd = I.x*en.x + I.y*en.y + I.z*en.z;
                Vector3 R(I.x - 2*dd*en.x, I.y - 2*dd*en.y, I.z - 2*dd*en.z);
                float mm = 2.0f * sqrtf(R.x*R.x + R.y*R.y + (R.z+1.0f)*(R.z+1.0f));
                if (mm < 1e-5f) mm = 1e-5f;
                u[k] = R.x / mm + 0.5f;
                v[k] = 1.0f - (R.y / mm + 0.5f); // flip-V para matchear el GL_SPHERE_MAP del PC (texture matrix)
            }
        }
        if (equirect) {
            // COSTURA: si los corners NO-polo cruzan la costura (rango U > 0.5), suben 1.0 (GL_REPEAT -> continuo)
            float umin = 2.0f, umax = -1.0f;
            for (int k = 0; k < 3; k++) if (!polo[k]) { if (u[k] < umin) umin = u[k]; if (u[k] > umax) umax = u[k]; }
            if (umax > umin && umax - umin > 0.5f)
                for (int k = 0; k < 3; k++) if (!polo[k] && u[k] < 0.5f) u[k] += 1.0f;
            // POLO: al corner del polo le damos la U PROMEDIO de los no-polo -> sin "batidora"
            float sum = 0.0f; int n = 0;
            for (int k = 0; k < 3; k++) if (!polo[k]) { sum += u[k]; n++; }
            if (n > 0) { float avg = sum / n; for (int k = 0; k < 3; k++) if (polo[k]) u[k] = avg; }
        }
        // guardar los 3 corners (posicion LOCAL + UV)
        for (int k = 0; k < 3; k++) {
            int c = t + k;
            chromeExpPos[c*3] = (GLfloat)lp[k].x; chromeExpPos[c*3+1] = (GLfloat)lp[k].y; chromeExpPos[c*3+2] = (GLfloat)lp[k].z;
            chromeExpUV[c*2] = u[k]; chromeExpUV[c*2+1] = v[k];
        }
    }
    chromeCacheCam = cam;
    for (int i = 0; i < 16; i++) chromeCacheW[i] = W.m[i];
    chromeCacheEq = equirect;
    chromeUVValid = true;
}

// ===================================================
// aplica TODO el estado GL de un material, leyendolo DEL material (nada
// hardcodeado). RenderObject la llama solo cuando el material cambia.
void Mesh::AplicarMaterial(Material* mat, bool conLuz, bool solido) {
    namespace gfx = w3dEngine;
    gfx::SmoothShading(mat->smooth);
    gfx::Material(gfx::MatAmbient,  mat->ambient);
    gfx::Material(gfx::MatDiffuse,  mat->diffuse);
    gfx::Material(gfx::MatSpecular, mat->specular);
    gfx::Material(gfx::MatEmission, mat->emission);
    gfx::MaterialShininess(mat->shininess);

    // color por vertice (via ColorMaterial) o el difuso plano del material
    if (mat->vertexColor && vertexColor) {
        gfx::Color4f(0.0f, 0.0f, 0.0f, 1.0f); // el color real lo pone el array
        gfx::EnableArray(gfx::ColorArray);
        gfx::Enable(gfx::ColorMaterial);
    } else {
        // con NORMAL MAP la base va sin luz -> la tiño con el COLOR de la luz aca (sino el N.L sale BLANCO).
        if (mat->normalMap)
            gfx::Color4f(mat->diffuse[0]*g_renderLightColor.x, mat->diffuse[1]*g_renderLightColor.y,
                         mat->diffuse[2]*g_renderLightColor.z, mat->diffuse[3]);
        else
            gfx::Color4f(mat->diffuse[0], mat->diffuse[1], mat->diffuse[2], mat->diffuse[3]);
        gfx::DisableArray(gfx::ColorArray);
        gfx::Disable(gfx::ColorMaterial);
    }

    // textura (nunca en Solid; respeta el checkbox textureOn del material)
    if (!solido && mat->texture && mat->textureOn) {
        gfx::Enable(gfx::Texture2D);
        gfx::BindTexture(mat->texture->iID);
        gfx::TexFilter(mat->filtrado);
        gfx::TexWrap(mat->repeat);
        // REFLECTION 3 modos (mat->reflectMode):
        //  0 MATCAP   = normal-del-ojo por MATRIZ DE TEXTURA -> HARDWARE en PC Y N95 (rapido). Normales como texcoords.
        //  1 SPHEREMAP= GL_SPHERE_MAP exacto -> HARDWARE en PC (texgen); en N95 (sin texgen) cae a SOFTWARE.
        //  2 EQUIRECT = 360 -> SIEMPRE por SOFTWARE (UV por-corner en CPU, calidad).
        bool matcap     = mat->chrome && mat->reflectMode == 0;
        bool sphereExact= mat->chrome && mat->reflectMode == 1;
        bool eq         = mat->chrome && mat->reflectMode == 2;
        bool sphereHW   = sphereExact && gfx::TieneTexGen();      // PC: sphere exacto por texgen
        bool sw         = eq || (sphereExact && !gfx::TieneTexGen()); // SOFTWARE: equirect siempre + sphere exacto en N95
        // OJO: TexGenSphere y TexMatrixMatcap tocan los DOS la matriz de textura -> nunca los dos a la vez.
        if (matcap) { gfx::TexGenSphere(false); gfx::TexMatrixMatcap(true); }   // matcap: matriz de textura (HW)
        else        { gfx::TexMatrixMatcap(false); gfx::TexGenSphere(sphereHW); } // sphere HW (flip-V) o reset
        gfx::TexEnvReplace(mat->chrome);        // reflejo (cualquier modo) = espejo: textura directa, sin luz
        if (matcap) { gfx::EnableArray(gfx::TexCoordArray); gfx::TexCoordPointer3b(normals); } // normales -> texcoords
        else if (sw) { ActualizarChromeUV(eq); // build los arrays por-corner (equirect o sphere); bind/draw en el loop
                  gfx::EnableArray(gfx::TexCoordArray); if (eq) gfx::TexWrap(true); } // REPEAT solo equirect (costura)
        else if (uv) gfx::TexCoordPointer2f(0, uv); // restaura las UV del modelo (el sphere HW usa texgen igual)
    } else {
        gfx::Disable(gfx::Texture2D);
        gfx::TexMatrixMatcap(false);
        gfx::TexGenSphere(false);
        gfx::TexEnvReplace(false);
        if (uv) gfx::TexCoordPointer2f(0, uv);
    }

    // normales: las sube la LUZ y el SPHERE-MAP por HARDWARE (texgen genera las UV de la normal). El MATCAP por
    // matriz de textura usa las normales como TEXCOORDS (no como NormalArray). El path por SOFTWARE (equirect, y el
    // sphere exacto del N95) NO usa el array (UV precomputadas + draw NO indexado). Por eso el reflejo no depende de la luz.
    bool sphereHWn = mat->chrome && mat->reflectMode == 1 && gfx::TieneTexGen();
    if (normals && (sphereHWn || (conLuz && mat->lighting))) gfx::EnableArray(gfx::NormalArray);
    else gfx::DisableArray(gfx::NormalArray);
    // la iluminacion en si solo si el material la pide (el chrome con GL_REPLACE la ignora -> espejo perfecto).
    // Con NORMAL MAP la base va SIN luz (albedo plano): el pass DOT3 de abajo aporta toda la iluminacion (N.L).
    if (conLuz && mat->lighting && !mat->normalMap) gfx::Enable(gfx::Lighting);
    else gfx::Disable(gfx::Lighting);

    if (mat->culling)     gfx::Enable(gfx::CullFace);  else gfx::Disable(gfx::CullFace);
    if (mat->depth_test)  gfx::Enable(gfx::DepthTest); else gfx::Disable(gfx::DepthTest);
    if (mat->transparent) { gfx::Enable(gfx::Blend); gfx::BlendAlpha(); }
    else                    gfx::Disable(gfx::Blend);
}

// hook de overlays del editor (lo registra el editor al arrancar; NULL en una app sin editor)
void (*g_meshOverlayHook)(Mesh*) = NULL;

void Mesh::RenderObject() {
    const bool editActiva = ((Object*)this == g_editMesh); // esta malla en Edit Mode
    // sin vertices no hay nada. Sin CARAS (todas borradas en Edit) igual hay que
    // dibujar el edit mesh (vertices + bordes sueltos), asi que en Edit NO cortamos.
    if (!vertex || vertexSize <= 0) return;
    if ((!faces || facesSize < 3) && !editActiva) return;
    // el material por defecto SIEMPRE tiene que existir (en Symbian arranca NULL)
    if (!MaterialDefecto) MaterialDefecto = new Material("Default", true);
    namespace gfx = w3dEngine;
    // el viewport que llama a esto todavia toca GL directo: resincronizamos el
    // cache de estado del motor (asi sus Enable/Disable ahorran llamadas).
    gfx::Invalidate();

    // punteros de los datos de la malla (una sola vez)
    gfx::VertexPointer3f(0, vertex);
    if (normals)     gfx::NormalPointer3b(normals);
    if (vertexColor) gfx::ColorPointer4ub(vertexColor);
    if (uv) { gfx::EnableArray(gfx::TexCoordArray); gfx::TexCoordPointer2f(0, uv); }

    // WIREFRAME: usa los BORDES precalculados (mas barato que el wireframe de
    // triangulos). Verde si esta seleccionada, gris si no. Sin bordes -> fallback.
    if (view == RenderType::Wireframe) {
        if (editActiva && g_mostrarOverlays) {
            // en wireframe NO hay relleno que tape el fondo: el overlay de edicion
            // (lineas con vertex color + vertices) se ve entero (todos los puntos).
            // sin overlays (limpieza de pantalla) cae al wireframe plano de abajo.
            RenderEditOverlay();
        } else {
        int cid = !select ? RC_wireframe
                          : (((Object*)this == ObjActivo) ? RC_selActive : RC_selInactive);
        const float* col = gRenderColors[cid];
        if (!edges.empty()) {
            RenderBordes(col, select ? 2.0f : 1.0f, false);
        } else {
            gfx::Disable(gfx::Lighting); gfx::Disable(gfx::Texture2D);
            gfx::Disable(gfx::Blend); gfx::Disable(gfx::CullFace);
            gfx::DisableArray(gfx::ColorArray);
            gfx::Color4f(col[0], col[1], col[2], 1.0f);
            gfx::Wireframe(true);
            gfx::DrawTriangles(facesSize, faces);
            gfx::Wireframe(false);
        }
        }
    } else {
        // un dibujo por grupo. El material se aplica SOLO cuando CAMBIA (en Solid
        // es siempre el mismo -> una vez). Solid = material por defecto sin
        // texturas; ZBuffer = sin luz (solo profundidad).
        const bool solido = (view == RenderType::Solid);
        const bool conLuz = (view != RenderType::ZBuffer);
        Material* ultimo = NULL;
        bool nmListo = false; // los nmColors (L en tangent-space) se calculan UNA vez por frame

        // MALLA GENERADA por modificadores: se dibuja el PREVIEW en Object Y en Edit Mode (real-time; en Edit el
        // overlay de vertices/aristas -editable- se dibuja ENCIMA -> editas el original y ves el resultado). En Edit,
        // GenerarMallaModificada saltea los modificadores con mostrarEdit=false (edicion mas rapida, N95). Draw SIMPLE.
        const bool useGen = (genValido && genVertex && genFaces);
        if (useGen) {
            gfx::VertexPointer3f(0, genVertex);
            if (genNormals) gfx::NormalPointer3b(genNormals);
            if (genColor)   gfx::ColorPointer4ub(genColor);
            if (genUV) { gfx::EnableArray(gfx::TexCoordArray); gfx::TexCoordPointer2f(0, genUV); }
        }

        // glPolygonOffset (slope-aware) sobre los RELLENOS:
        //  - object mode seleccionado: rellenos MUY adelantados -> el borde queda
        //    bien ATRAS, no se dibuja sobre la malla.
        //  - edit mode: rellenos un toque atras (decal) -> las lineas/puntos a
        //    profundidad normal quedan ENCIMA del frente y el fondo lo tapa la malla.
        if (editActiva) { gfx::Enable(gfx::PolygonOffsetFill); gfx::PolygonOffset(2.0f, 4.0f); }
        else if (select) { gfx::Enable(gfx::PolygonOffsetFill); gfx::PolygonOffset(-4.0f, -8.0f); }

        size_t ng = useGen ? genMaterialsGroup.size() : materialsGroup.size();
        for (size_t g = 0; g < ng; g++) {
            const MaterialGroup& grp = useGen ? genMaterialsGroup[g] : materialsGroup[g];
            Material* mat = grp.material;
            if (solido || !mat) mat = MaterialDefecto;
            if (mat != ultimo) { AplicarMaterial(mat, conLuz, solido); ultimo = mat; }
            if (useGen) { // malla generada: draw indexado simple (v1 sin chrome/normalmap/capas extra)
                gfx::DrawTriangles(grp.indicesDrawnCount, &genFaces[grp.startDrawn]);
                continue;
            }
            // REFLECTION por SOFTWARE (equirect SIEMPRE, o sphere exacto en GLES1/N95): render NO INDEXADO por-corner.
            // El MATCAP (matriz de textura) y el sphere HW van por el draw INDEXADO normal (else) -> no entran aca.
            if (!solido && mat->chrome && (mat->reflectMode == 2 || (mat->reflectMode == 1 && !gfx::TieneTexGen())) && chromeExpPos && chromeExpUV) {
                gfx::VertexPointer3f(0, chromeExpPos);
                gfx::TexCoordPointer2f(0, chromeExpUV);
                gfx::DrawTrianglesArrayFrom(materialsGroup[g].startDrawn, materialsGroup[g].indicesDrawnCount);
                gfx::VertexPointer3f(0, vertex); // re-bindea las posiciones INDEXADAS para el resto/proximo grupo
                ultimo = NULL;                   // el puntero de UV cambio -> re-AplicarMaterial el proximo grupo
            } else {
                gfx::DrawTriangles(materialsGroup[g].indicesDrawnCount,
                                   &faces[materialsGroup[g].startDrawn]);
            }

            // NORMAL MAPPING (DOT3) — pass 2 sobre la base: textura normal + N.L (color=L por vertice) en blend
            // MULTIPLY -> base * (N.L). Textura UNICA (sin multitextura) -> portable PC + N95. Excluyente con chrome.
            if (!solido && mat->normalMap && mat->normalTexture && mat->normalTexture->iID && uv) {
                CalcularTangentes();
                if (tangents) {
                    if (!nmListo) { ActualizarNormalMapColors(g_renderLightPos); nmListo = true; } // N.L con la LUZ de la escena
                    gfx::Enable(gfx::Blend); gfx::BlendMode(1);             // Multiply (oscurece por N.L)
                    gfx::DepthFunc(gfx::DepthEqual); gfx::DepthMask(false); // misma superficie, NO re-escribe z
                    gfx::Disable(gfx::Lighting); gfx::DisableArray(gfx::NormalArray);
                    gfx::Disable(gfx::ColorMaterial);
                    gfx::Enable(gfx::Texture2D);
                    gfx::BindTexture(mat->normalTexture->iID);
                    gfx::TexFilter(mat->filtrado); gfx::TexWrap(mat->repeat);
                    gfx::TexEnvDot3(true);                                  // combiner N.L
                    gfx::EnableArray(gfx::ColorArray); gfx::ColorPointer4ub(nmColors); // L como primary color
                    gfx::TexCoordPointer2f(0, uv);                         // mismas UV que la base
                    gfx::DrawTriangles(materialsGroup[g].indicesDrawnCount, &faces[materialsGroup[g].startDrawn]);
                    gfx::TexEnvDot3(false);
                    gfx::DepthFunc(gfx::DepthLess); gfx::DepthMask(true);
                    gfx::Disable(gfx::Blend); gfx::BlendAlpha();
                    ultimo = NULL; // cambio textura/color/blend -> re-AplicarMaterial el proximo grupo
                }
            }

            // CAPAS EXTRA (multi-pass, eficiente: 1 draw por capa sobre la MISMA superficie con su blend).
            // Comparten el UV del modelo. GL 1.1 -> anda igual en PC y N95 (sin multitextura/extensiones).
            if (!solido && !mat->capas.empty() && uv) {
                gfx::Enable(gfx::Blend);
                gfx::DepthFunc(gfx::DepthEqual); gfx::DepthMask(false); // misma superficie, NO re-escribe z
                for (size_t c = 0; c < mat->capas.size(); c++) {
                    const TexLayer& cap = mat->capas[c];
                    if (!cap.on || !cap.tex) continue;
                    gfx::Enable(gfx::Texture2D);
                    gfx::BindTexture(cap.tex->iID);
                    gfx::BlendMode(cap.blend); // Mix / Multiply / Add
                    gfx::DrawTriangles(materialsGroup[g].indicesDrawnCount, &faces[materialsGroup[g].startDrawn]);
                }
                gfx::DepthFunc(gfx::DepthLess); gfx::DepthMask(true);
                gfx::Disable(gfx::Blend); gfx::BlendAlpha(); // restaura el blend func default
                ultimo = NULL; // la textura/blend cambio -> re-AplicarMaterial el proximo grupo
            }
        }

        gfx::Disable(gfx::PolygonOffsetFill);
        gfx::TexMatrixMatcap(false); // CLAVE N95: resetea la matriz de textura del MATCAP. En PC el TexGenSphere(false)
                                     // de abajo la limpiaba de paso (hace glLoadIdentity), pero en el N95 TexGenSphere
                                     // es un stub (no hay texgen) -> la matriz quedaba sucia -> la UI texturada (fuente/
                                     // iconos) salia con texcoords transformadas = INVISIBLE. Sirve en los 4 OS.
        gfx::TexGenSphere(false); // resetea el chrome (que no leakee al contorno/overlays/proxima malla)
        gfx::TexEnvReplace(false); // vuelve a GL_MODULATE: sino la UI/fuente quedan sin tinte de color
        gfx::DepthFunc(gfx::DepthLess);

        // overlays (contorno de seleccion / normales / overlay de edit): los dibuja el EDITOR
        // via hook, JUSTO tras el relleno (mismo timing que antes). NULL en una app sin editor.
        if (g_meshOverlayHook) g_meshOverlayHook(this);
    }
}

// angulo (rad) en el vertice p del triangulo p-q-r. Pondera la normal de cara
// en el vertex-normal: asi un quad (2 triangulos) cuenta como UNA cara y el
// promedio da 45 grados en las esquinas del cubo (no sesgado por la diagonal).




// recalcula los BORDES unicos desde faces3d, dedup por POSICION (no por indice:
// asi un borde compartido por 2 caras con vertices distintos -mismo lugar- no se
// repite). Cada par (edges[2i], edges[2i+1]) es una arista.
void Mesh::CalcularBordes(bool invalidarEdit) {
    edges.clear();
    posRep.clear();
    bordesBuf.clear();
    vertsAgrupados = 0;
    overlayLcache = -1.0f; // la geometria cambio -> rehacer los overlays de normales
    if (!vertex || vertexSize <= 0) return;
    const int nV = vertexSize;
    // representante de cada vertice por posicion: el menor indice entre los verts coincidentes. HASH por posicion
    // CUANTIZADA a 1e-4 -> O(n log n). (Antes era un doble loop O(n^2) que con una esfera de 45+ segmentos TRABABA
    // la app al regenerar -en el redo panel el slider regenera muchas veces-: Mismo resultado, mucho mas
    // rapido: a 64x64 son ~4k verts -> 4k*log4k en vez de 16M comparaciones.) Se cachea en posRep (overlay O(n)/frame).
    posRep.assign(nV, 0);
    std::map<std::string,int> posMap;
    for (int i = 0; i < nV; i++) {
        int q[3];
        q[0] = (int)floorf(vertex[i*3]   * 10000.0f + 0.5f); // cuantiza a 1e-4 (coincidentes -> misma celda)
        q[1] = (int)floorf(vertex[i*3+1] * 10000.0f + 0.5f);
        q[2] = (int)floorf(vertex[i*3+2] * 10000.0f + 0.5f);
        std::string key((const char*)q, sizeof(q));
        std::map<std::string,int>::iterator it = posMap.find(key);
        if (it != posMap.end()) posRep[i] = it->second; // ya vimos esta posicion -> su representante (menor indice)
        else { posRep[i] = i; posMap[key] = i; }         // 1ra vez -> este vert es el representante
    }
    // cantidad de posiciones unicas + CENTRO GEOMETRICO (promedio de las posiciones
    // unicas = los grupos de vertice). El foco/pivot lo usan en vez del origen.
    float cgx = 0, cgy = 0, cgz = 0;
    for (int i = 0; i < nV; i++) if (posRep[i] == i) {
        vertsAgrupados++;
        cgx += vertex[i*3]; cgy += vertex[i*3+1]; cgz += vertex[i*3+2];
    }
    if (vertsAgrupados > 0)
        centroGeom = Vector3(cgx / vertsAgrupados, cgy / vertsAgrupados, cgz / vertsAgrupados);
    else
        centroGeom = Vector3(0, 0, 0);

    // RADIO del bounding LOCAL alrededor de centroGeom (la distancia mas lejana). Lo usa el foco/encuadre
    // (tecla '.') para ajustar el zoom a lo que se ve. Se recalcula solo cuando cambia la geometria.
    float rg2 = 0.0f;
    for (int i = 0; i < nV; i++) {
        float dx = vertex[i*3] - centroGeom.x, dy = vertex[i*3+1] - centroGeom.y, dz = vertex[i*3+2] - centroGeom.z;
        float d2 = dx*dx + dy*dy + dz*dz; if (d2 > rg2) rg2 = d2;
    }
    radioGeom = sqrtf(rg2);

    // la geometria cambio -> la malla de EDICION (si existia) queda invalida.
    // (salvo el confirm del transform de malla: solo movio vertices, conserva el
    // edit y su seleccion -> invalidarEdit=false)
    if (invalidarEdit) InvalidarEdit();
    if (faces3d.empty() && looseEdges.empty()) return; // sin caras ni bordes sueltos
    std::vector<int>& rep = posRep;
    std::set<std::pair<int,int> > unicos;
    for (size_t f = 0; f < faces3d.size(); f++) {
        const std::vector<int>& idx = faces3d[f].idx;
        int m = (int)idx.size();
        for (int k = 0; k < m; k++) {
            int a = idx[k], b = idx[(k+1)%m];
            if (a<0||a>=nV||b<0||b>=nV) continue;
            int ra = rep[a], rb = rep[b];
            if (ra == rb) continue;
            if (ra > rb) { int t=ra; ra=rb; rb=t; }
            unicos.insert(std::make_pair(ra, rb));
        }
    }
    // BORDES SUELTOS (del borrado en edit): dedup por POSICION igual que los de cara
    for (size_t e = 0; e + 1 < looseEdges.size(); e += 2) {
        int a = looseEdges[e], b = looseEdges[e+1];
        if (a<0||a>=nV||b<0||b>=nV) continue;
        int ra = rep[a], rb = rep[b];
        if (ra == rb) continue;
        if (ra > rb) { int t=ra; ra=rb; rb=t; }
        unicos.insert(std::make_pair(ra, rb));
    }
    edges.reserve(unicos.size()*2);
    for (std::set<std::pair<int,int> >::iterator it = unicos.begin(); it != unicos.end(); ++it) {
        edges.push_back(it->first);
        edges.push_back(it->second);
    }
    // precalcular el buffer de lineas del contorno (asi NO se rehace por frame)
    bordesBuf.reserve(edges.size()*3);
    for (size_t e = 0; e + 1 < edges.size(); e += 2) {
        int a = edges[e], b = edges[e+1];
        bordesBuf.push_back(vertex[a*3]); bordesBuf.push_back(vertex[a*3+1]); bordesBuf.push_back(vertex[a*3+2]);
        bordesBuf.push_back(vertex[b*3]); bordesBuf.push_back(vertex[b*3+1]); bordesBuf.push_back(vertex[b*3+2]);
    }
}

// recalcula el array 'normals' (GLbyte por vertice GPU) desde la geometria actual:
// normal Newell de cada cara logica (faces3d) acumulada en sus corners; si la malla
// es SMOOTH se promedia agrupando por POSICION (posRep). Lo llama el transform de
// malla al CONFIRMAR (mover vertices invalida las normales viejas).
void Mesh::RecalcularNormales() {
    if (!vertex || !normals || vertexSize <= 0) return;
    const int nV = vertexSize;
    std::vector<float> acc(nV*3, 0.0f);
    if (!faces3d.empty()) {
        for (size_t fi = 0; fi < faces3d.size(); fi++) {
            const std::vector<int>& ring = faces3d[fi].idx;
            int m = (int)ring.size(); if (m < 3) continue;
            float nx=0,ny=0,nz=0;
            for (int k=0;k<m;k++){ // Newell (robusto para ngones)
                GLfloat* a=&vertex[ring[k]*3]; GLfloat* b=&vertex[ring[(k+1)%m]*3];
                nx+=(a[1]-b[1])*(a[2]+b[2]); ny+=(a[2]-b[2])*(a[0]+b[0]); nz+=(a[0]-b[0])*(a[1]+b[1]);
            }
            float ln=sqrtf(nx*nx+ny*ny+nz*nz); if (ln<1e-6f) continue; nx/=ln; ny/=ln; nz/=ln;
            for (int k=0;k<m;k++){ int gi=ring[k]; if (gi<0||gi>=nV) continue;
                acc[gi*3]+=nx; acc[gi*3+1]+=ny; acc[gi*3+2]+=nz; }
        }
    } else {
        int nTri = facesSize/3;
        for (int t=0;t<nTri;t++){
            int i0=faces[t*3], i1=faces[t*3+1], i2=faces[t*3+2];
            GLfloat* p0=&vertex[i0*3]; GLfloat* p1=&vertex[i1*3]; GLfloat* p2=&vertex[i2*3];
            float ax=p1[0]-p0[0], ay=p1[1]-p0[1], az=p1[2]-p0[2];
            float bx=p2[0]-p0[0], by=p2[1]-p0[1], bz=p2[2]-p0[2];
            float nx=ay*bz-az*by, ny=az*bx-ax*bz, nz=ax*by-ay*bx;
            float ln=sqrtf(nx*nx+ny*ny+nz*nz); if (ln<1e-6f) continue; nx/=ln; ny/=ln; nz/=ln;
            acc[i0*3]+=nx; acc[i0*3+1]+=ny; acc[i0*3+2]+=nz;
            acc[i1*3]+=nx; acc[i1*3+1]+=ny; acc[i1*3+2]+=nz;
            acc[i2*3]+=nx; acc[i2*3+1]+=ny; acc[i2*3+2]+=nz;
        }
    }
    // SMOOTH: promedio por POSICION (los vertices del mismo lugar comparten normal)
    if (meshSmooth && (int)posRep.size() == nV) {
        std::vector<float> sum(nV*3, 0.0f);
        for (int i=0;i<nV;i++){ int r=posRep[i]; sum[r*3]+=acc[i*3]; sum[r*3+1]+=acc[i*3+1]; sum[r*3+2]+=acc[i*3+2]; }
        for (int i=0;i<nV;i++){ int r=posRep[i]; acc[i*3]=sum[r*3]; acc[i*3+1]=sum[r*3+1]; acc[i*3+2]=sum[r*3+2]; }
    }
    for (int i=0;i<nV;i++){
        float nx=acc[i*3], ny=acc[i*3+1], nz=acc[i*3+2];
        float ln=sqrtf(nx*nx+ny*ny+nz*nz);
        if (ln<1e-6f){ normals[i*3]=0; normals[i*3+1]=127; normals[i*3+2]=0; continue; }
        nx/=ln; ny/=ln; nz/=ln;
        normals[i*3]=(GLbyte)(nx*127.0f); normals[i*3+1]=(GLbyte)(ny*127.0f); normals[i*3+2]=(GLbyte)(nz*127.0f);
    }
    SincronizarCornerNormal(); // -> capa autoritativa por corner (la que lee GenerarRender)
}

// copia normals[] (render) -> cornerNormal (autoritativa POR CORNER). La llaman las ops
// que ESCRIBEN normales (RecalcularNormales, shade) para que GenerarRender las vea.
void Mesh::SincronizarCornerNormal() {
    if (!normals || faces3d.empty()) return;
    int nC = ContarCorners(); cornerNormal.resize((size_t)nC*3);
    int L=0; for (size_t f=0;f<faces3d.size();f++) for (size_t c=0;c<faces3d[f].idx.size();c++){
        int gv=faces3d[f].idx[c]; if (gv<0||gv>=vertexSize){L++;continue;}
        cornerNormal[L*3]=normals[gv*3]; cornerNormal[L*3+1]=normals[gv*3+1]; cornerNormal[L*3+2]=normals[gv*3+2]; L++; }
}

// FLAT: cada corner toma la normal Newell de SU cara (no se acarrea/promedia). Asi en
// GenerarRender dos corners en la misma posicion pero de caras distintas tienen normales
// distintas -> NO se mergean -> cada cara queda con su propia normal (shading plano). Lo
// que hacia smooth al extruir un cubo: las paredes heredaban la normal de la cara original.
void Mesh::CornerNormalPorCara() {
    if (!vertex || faces3d.empty()) return;
    int nC = ContarCorners();
    cornerNormal.resize((size_t)nC*3);
    int L = 0;
    for (size_t f=0; f<faces3d.size(); f++) {
        const std::vector<int>& ring = faces3d[f].idx;
        int m = (int)ring.size();
        float nx=0,ny=0,nz=0;
        for (int k=0;k<m;k++){ // Newell (robusto para ngones / quads no planos)
            int ia=ring[k], ib=ring[(k+1)%m];
            if (ia<0||ia>=vertexSize||ib<0||ib>=vertexSize) continue;
            GLfloat* a=&vertex[ia*3]; GLfloat* b=&vertex[ib*3];
            nx+=(a[1]-b[1])*(a[2]+b[2]); ny+=(a[2]-b[2])*(a[0]+b[0]); nz+=(a[0]-b[0])*(a[1]+b[1]);
        }
        float ln=sqrtf(nx*nx+ny*ny+nz*nz);
        GLbyte bx=0,by=127,bz=0; // degenerada -> +Y de fallback
        if (ln>=1e-6f){ nx/=ln;ny/=ln;nz/=ln; bx=(GLbyte)(nx*127.0f); by=(GLbyte)(ny*127.0f); bz=(GLbyte)(nz*127.0f); }
        for (int k=0;k<m;k++){ if (L*3+2 < (int)cornerNormal.size()){ cornerNormal[L*3]=bx; cornerNormal[L*3+1]=by; cornerNormal[L*3+2]=bz; } L++; }
    }
}

// clave de 12 bytes de una posicion (los 3 floats crudos). Dos corners en el MISMO
// lugar tienen exactamente los mismos bytes -> mismo grupo de posicion.
static std::string PosKey12(const float* p){ char b[12]; memcpy(b, p, 12); return std::string(b, 12); }

std::string Mesh::SharpEdgeKey(const float* a, const float* b){
    std::string ka = PosKey12(a), kb = PosKey12(b);
    return (ka < kb) ? (ka + kb) : (kb + ka); // ordenado: el borde no tiene direccion
}

// union-find (raices de los grupos de suavizado, indexado por corner)
static int UFFind(std::vector<int>& uf, int x){ while (uf[x] != x){ uf[x] = uf[uf[x]]; x = uf[x]; } return x; }
static void UFUnion(std::vector<int>& uf, int a, int b){ int ra=UFFind(uf,a), rb=UFFind(uf,b); if (ra!=rb) uf[ra]=rb; }

// SMOOTH con bordes SHARP: la normal de cada corner = promedio de las caras de su GRUPO
// DE SUAVIZADO. El grupo se arma uniendo, alrededor de cada vertice, las caras que
// comparten un borde NO sharp; un borde sharp (o toda la malla flat) CORTA el grupo ->
// esa cara queda con su propia normal (plana). Asi un cilindro shade-smooth con el aro
// de las tapas marcado sharp: lados suaves + tapas planas + arista filosa.
void Mesh::CornerNormalConSharp(){
    if (!vertex || faces3d.empty()) return;
    int nC = ContarCorners();
    if (nC <= 0) return;
    cornerNormal.resize((size_t)nC * 3);

    // 1) normal Newell por cara + de que cara es cada corner + si la cara es FLAT (per-cara: -1 hereda meshSmooth)
    std::vector<float> fn(faces3d.size() * 3, 0.0f);
    std::vector<int> cornerFace(nC, -1);
    std::vector<char> faceFlat(faces3d.size(), 0);
    for (size_t f=0; f<faces3d.size(); f++){ int sm=faces3d[f].smooth; faceFlat[f] = (sm==0) || (sm<0 && !meshSmooth) ? 1 : 0; }
    std::vector<int> uf(nC); for (int i = 0; i < nC; i++) uf[i] = i;
    int L = 0;
    for (size_t f = 0; f < faces3d.size(); f++) {
        const std::vector<int>& ring = faces3d[f].idx;
        int m = (int)ring.size();
        float nx=0, ny=0, nz=0;
        for (int k = 0; k < m; k++) {
            int ia = ring[k], ib = ring[(k+1)%m];
            if (ia<0||ia>=vertexSize||ib<0||ib>=vertexSize) continue;
            GLfloat* a=&vertex[ia*3]; GLfloat* b=&vertex[ib*3];
            nx += (a[1]-b[1])*(a[2]+b[2]); ny += (a[2]-b[2])*(a[0]+b[0]); nz += (a[0]-b[0])*(a[1]+b[1]);
        }
        float ln = sqrtf(nx*nx+ny*ny+nz*nz); if (ln>1e-6f){ nx/=ln; ny/=ln; nz/=ln; } else { nx=0; ny=1; nz=0; }
        fn[f*3]=nx; fn[f*3+1]=ny; fn[f*3+2]=nz;
        for (int k = 0; k < m; k++){ if (L < nC) cornerFace[L]=(int)f; L++; }
    }

    // 2) por cada borde compartido (key por posicion) unir los corners de las 2 caras en
    //    cada extremo, SALVO que sea sharp (o la malla flat = todo sharp -> nunca une)
    std::map<std::string, std::pair<int,int> > visto; // key -> (cornerEnBajo, cornerEnAlto) de la 1er cara
    L = 0;
    for (size_t f = 0; f < faces3d.size(); f++) {
        const std::vector<int>& ring = faces3d[f].idx;
        int m = (int)ring.size();
        for (int k = 0; k < m; k++) {
            int ia = ring[k], ib = ring[(k+1)%m];
            int ca = L + k, cb = L + ((k+1)%m);     // corners en ia, ib
            if (ia<0||ia>=vertexSize||ib<0||ib>=vertexSize) continue;
            std::string pkA = PosKey12(&vertex[ia*3]);
            std::string pkB = PosKey12(&vertex[ib*3]);
            if (pkA == pkB) continue;                // arista degenerada
            int loC, hiC; std::string ek;
            if (pkA < pkB){ loC=ca; hiC=cb; ek = pkA + pkB; }
            else          { loC=cb; hiC=ca; ek = pkB + pkA; }
            std::map<std::string, std::pair<int,int> >::iterator it = visto.find(ek);
            if (it == visto.end()) { visto[ek] = std::make_pair(loC, hiC); }
            else {
                // borde sharp si esta marcado, o si CUALQUIERA de las 2 caras del borde es flat (per-cara) -> corta el grupo
                int fOtra = cornerFace[it->second.first];
                bool sharp = (sharpEdges.find(ek) != sharpEdges.end()) || faceFlat[f] || (fOtra>=0 && faceFlat[fOtra]);
                if (!sharp) { UFUnion(uf, it->second.first, loC); UFUnion(uf, it->second.second, hiC); }
            }
        }
        L += m;
    }

    // 3) acumular la normal de cada grupo (raiz) y escribirla en sus corners
    std::vector<float> acc(nC * 3, 0.0f);
    for (int c = 0; c < nC; c++){ int r = UFFind(uf, c); int f = cornerFace[c]; if (f < 0) continue;
        acc[r*3]+=fn[f*3]; acc[r*3+1]+=fn[f*3+1]; acc[r*3+2]+=fn[f*3+2]; }
    for (int c = 0; c < nC; c++){ int r = UFFind(uf, c);
        float nx=acc[r*3], ny=acc[r*3+1], nz=acc[r*3+2];
        float ln=sqrtf(nx*nx+ny*ny+nz*nz);
        GLbyte bx=0, by=127, bz=0;
        if (ln>1e-6f){ nx/=ln; ny/=ln; nz/=ln; bx=(GLbyte)(nx*127.0f); by=(GLbyte)(ny*127.0f); bz=(GLbyte)(nz*127.0f); }
        cornerNormal[c*3]=bx; cornerNormal[c*3+1]=by; cornerNormal[c*3+2]=bz; }
}

// dibuja el buffer de bordes PRECALCULADO (bordesBuf) como GL_LINES. No arma nada
// por frame: solo color + DrawLines. Lo usan el contorno de seleccion y el wireframe.
void Mesh::RenderBordes(const float* color, float width, bool pushBack) {
    if (bordesBuf.empty() || !vertex) return;
    namespace gfx = w3dEngine;
    gfx::Disable(gfx::Lighting);
    gfx::Disable(gfx::Texture2D);
    gfx::DisableArray(gfx::NormalArray);
    gfx::DisableArray(gfx::ColorArray);
    gfx::DisableArray(gfx::TexCoordArray);
    gfx::Color4f(color[0], color[1], color[2], 1.0f);
    gfx::LineWidth(width);
    if (pushBack) gfx::DepthRange(0.0008f, 1.0f); // (el contorno usa glPolygonOffset)

    gfx::VertexPointer3f(0, &bordesBuf[0]);
    gfx::DrawLines((int)(bordesBuf.size()/3));

    if (pushBack) gfx::DepthRange(0.0f, 1.0f);
    gfx::LineWidth(1.0f);
    gfx::VertexPointer3f(0, vertex);
    gfx::Invalidate();
}


// foco/pivot de la malla = su CENTRO GEOMETRICO en mundo (no el origen)
Vector3 Mesh::PuntoFoco() const { return LocalAMundo(centroGeom); }

// escala un radio LOCAL (esfera de radio rLocal alrededor de cLocal) a MUNDO: transforma 3 puntos
// sobre los ejes y toma el mayor (asi una escala no uniforme no subestima el bounding).
float Mesh::EscalarRadioLocal(const Vector3& cLocal, float rLocal) const {
    Vector3 c = LocalAMundo(cLocal);
    Vector3 ax[3] = { Vector3(rLocal,0,0), Vector3(0,rLocal,0), Vector3(0,0,rLocal) };
    float r = 0.0f;
    for (int i = 0; i < 3; i++) { float d = (LocalAMundo(cLocal + ax[i]) - c).Length(); if (d > r) r = d; }
    return r;
}
float Mesh::RadioFoco() const { return EscalarRadioLocal(centroGeom, radioGeom); }

// --- helpers de generacion (acumuladores dinamicos) ---
// agrega un vertice (pos+normal+uv) y devuelve su indice
static int PushV(std::vector<GLfloat>& vp, std::vector<GLbyte>& vn, std::vector<GLfloat>& vu,
                 float x,float y,float z, float nx,float ny,float nz, float u,float v){
    int i = (int)(vp.size()/3);
    vp.push_back(x); vp.push_back(y); vp.push_back(z);
    vn.push_back((GLbyte)(nx*127.0f)); vn.push_back((GLbyte)(ny*127.0f)); vn.push_back((GLbyte)(nz*127.0f));
    vu.push_back(u); vu.push_back(v);
    return i;
}
// normal (Newell) de un poligono dado por m posiciones (robusto para ngones)
static void NewellPos(const float* pos, int m, float& nx, float& ny, float& nz){
    nx=ny=nz=0.0f;
    for (int k=0;k<m;k++){
        const float* a=&pos[k*3]; const float* b=&pos[((k+1)%m)*3];
        nx += (a[1]-b[1])*(a[2]+b[2]);
        ny += (a[2]-b[2])*(a[0]+b[0]);
        nz += (a[0]-b[0])*(a[1]+b[1]);
    }
    float ln=sqrtf(nx*nx+ny*ny+nz*nz); if (ln>1e-6f){ nx/=ln; ny/=ln; nz/=ln; }
}
// registra una cara logica (poligono) y la triangula en abanico desde ring[0]
static void AddFace(std::vector<GLushort>& tris, std::vector<MeshFace>& f3d, const std::vector<int>& ring){
    MeshFace mf; mf.idx = ring; f3d.push_back(mf);
    for (size_t k=1; k+1 < ring.size(); k++){
        tris.push_back((GLushort)ring[0]);
        tris.push_back((GLushort)ring[k]);
        tris.push_back((GLushort)ring[k+1]);
    }
}

// reconstruye la geometria de la primitiva desde meshTipo/meshSize/meshVerts.
// meshSize = span del cubo/plano o radio del circulo; meshVerts = vertices del
// circulo. La llama el panel "Add" al cambiar un parametro en vivo.
void Mesh::Regenerar(){
    Material* mat = materialsGroup.empty() ? MaterialDefecto : materialsGroup[0].material;
    delete[] vertex;      vertex = NULL;
    delete[] vertexColor; vertexColor = NULL;
    delete[] normals;     normals = NULL;
    delete[] uv;          uv = NULL;
    delete[] chromeExpPos; chromeExpPos = NULL;
    delete[] chromeExpUV;  chromeExpUV = NULL; chromeExpCount = 0; chromeUVValid = false;
    delete[] tangents; tangents = NULL; delete[] nmColors; nmColors = NULL; tangentsValid = false;
    delete[] faces;       faces = NULL;
    materialsGroup.clear();
    faces3d.clear();
    edges.clear();
    posRep.clear();
    bordesBuf.clear();
    normFaceBuf.clear(); normCustomBuf.clear(); normVertBuf.clear();
    overlayLcache = -1.0f;
    vertsAgrupados = 0;
    LiberarCapas();  // las capas (uv/color/groups) se rehacen para la geometria nueva
    InvalidarEdit(); // la malla de edicion se rehace on-demand

    int type = meshTipo;
    if (type == (int)MeshType::plane){
        // plano = UN quad (faces3d), horizontal (XZ), normal +Y
        float h = meshSize * 0.5f;
        std::vector<GLfloat> vp, vu; std::vector<GLbyte> vn; std::vector<GLushort> tris;
        static const float PC[4][3] = { {-1,0,1},{1,0,1},{1,0,-1},{-1,0,-1} }; // CCW -> +Y
        // V invertida (V=0 = ARRIBA de la imagen, como stb top-first + el importador OBJ que
        // hace 1-v): sino la textura sale dada vuelta verticalmente.
        static const float PUV[4][2] = { {0,1},{1,1},{1,0},{0,0} };
        std::vector<int> ring;
        for (int k=0;k<4;k++)
            ring.push_back(PushV(vp,vn,vu, PC[k][0]*h, 0.0f, PC[k][2]*h, 0,1,0, PUV[k][0],PUV[k][1]));
        AddFace(tris, faces3d, ring);
        vertexSize = (int)(vp.size()/3);
        vertex = new GLfloat[vp.size()]; for (size_t i=0;i<vp.size();i++) vertex[i]=vp[i];
        normals = new GLbyte[vn.size()]; for (size_t i=0;i<vn.size();i++) normals[i]=vn[i];
        uv = new GLfloat[vu.size()]; for (size_t i=0;i<vu.size();i++) uv[i]=vu[i];
        vertexColor = new GLubyte[vertexSize*4]; for (int i=0;i<vertexSize*4;i++) vertexColor[i]=255;
        facesSize = (int)tris.size();
        faces = new MeshIndex[facesSize>0?facesSize:1]; for (int i=0;i<facesSize;i++) faces[i]=tris[i];
    }
    else if (type == (int)MeshType::cube){
        // 6 caras QUAD (faces3d): cada una 1 cuadrado -> 1 normal cian. FLAT =
        // cada cara con sus 4 vertices y su normal; SMOOTH = 8 esquinas compartidas
        // con normal diagonal (cubo "redondeado").
        float h = meshSize * 0.5f;
        std::vector<GLfloat> vp, vu; std::vector<GLbyte> vn; std::vector<GLushort> tris;
        static const float CORN[8][3] = {
            {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
            {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1}
        };
        static const int CARA[6][4] = { // CCW visto desde afuera
            {4,5,6,7}, // +Z
            {1,0,3,2}, // -Z
            {5,1,2,6}, // +X
            {0,4,7,3}, // -X
            {7,6,2,3}, // +Y
            {0,1,5,4}  // -Y
        };
        // V invertida (V=0 = ARRIBA, como stb top-first + el importador OBJ 1-v): sino la textura del cubo sale dada vuelta verticalmente
        static const float UVQ[4][2] = { {0,1},{1,1},{1,0},{0,0} };
        if (meshSmooth){
            int idx[8];
            for (int c=0;c<8;c++){
                float nx=CORN[c][0], ny=CORN[c][1], nz=CORN[c][2];
                float ln=sqrtf(nx*nx+ny*ny+nz*nz); if(ln<1e-6f)ln=1.0f;
                idx[c]=PushV(vp,vn,vu, CORN[c][0]*h,CORN[c][1]*h,CORN[c][2]*h, nx/ln,ny/ln,nz/ln, 0.0f,0.0f);
            }
            for (int f=0;f<6;f++){
                std::vector<int> ring;
                for (int k=0;k<4;k++) ring.push_back(idx[CARA[f][k]]);
                AddFace(tris, faces3d, ring);
            }
        } else {
            for (int f=0;f<6;f++){
                float pos[12];
                for (int k=0;k<4;k++){
                    pos[k*3]   = CORN[CARA[f][k]][0]*h;
                    pos[k*3+1] = CORN[CARA[f][k]][1]*h;
                    pos[k*3+2] = CORN[CARA[f][k]][2]*h;
                }
                float fnx,fny,fnz; NewellPos(pos,4,fnx,fny,fnz);
                std::vector<int> ring;
                for (int k=0;k<4;k++)
                    ring.push_back(PushV(vp,vn,vu, pos[k*3],pos[k*3+1],pos[k*3+2], fnx,fny,fnz, UVQ[k][0],UVQ[k][1]));
                AddFace(tris, faces3d, ring);
            }
        }
        vertexSize = (int)(vp.size()/3);
        vertex = new GLfloat[vp.size()]; for (size_t i=0;i<vp.size();i++) vertex[i]=vp[i];
        normals = new GLbyte[vn.size()]; for (size_t i=0;i<vn.size();i++) normals[i]=vn[i];
        uv = new GLfloat[vu.size()]; for (size_t i=0;i<vu.size();i++) uv[i]=vu[i];
        vertexColor = new GLubyte[vertexSize*4]; for (int i=0;i<vertexSize*4;i++) vertexColor[i]=255;
        facesSize = (int)tris.size();
        faces = new MeshIndex[facesSize>0?facesSize:1]; for (int i=0;i<facesSize;i++) faces[i]=tris[i];
    }
    else if (type == (int)MeshType::circle){
        // disco = UN ngon (igual que las tapas del cono/cilindro): n vertices en
        // el borde, SIN vertice central, 1 cara -> 1 normal cian (+Y, mira arriba).
        int n = meshVerts; if (n < 3) n = 3;
        float R = meshSize; // radio
        std::vector<GLfloat> vp, vu; std::vector<GLbyte> vn; std::vector<GLushort> tris;
        std::vector<int> ring;
        for (int j = 0; j < n; j++){
            int jr = (n - j) % n; // orden invertido -> normal +Y (mira hacia arriba)
            float a = 2.0f * (float)M_PI * (float)jr / (float)n, ca = cosf(a), sa = sinf(a);
            ring.push_back(PushV(vp,vn,vu, R*ca, 0.0f, R*sa, 0,1,0, ca*0.5f+0.5f, sa*0.5f+0.5f));
        }
        AddFace(tris, faces3d, ring);
        vertexSize = (int)(vp.size()/3);
        vertex = new GLfloat[vp.size()]; for (size_t i=0;i<vp.size();i++) vertex[i]=vp[i];
        normals = new GLbyte[vn.size()]; for (size_t i=0;i<vn.size();i++) normals[i]=vn[i];
        uv = new GLfloat[vu.size()]; for (size_t i=0;i<vu.size();i++) uv[i]=vu[i];
        vertexColor = new GLubyte[vertexSize*4]; for (int i=0;i<vertexSize*4;i++) vertexColor[i]=255;
        facesSize = (int)tris.size();
        faces = new MeshIndex[facesSize>0?facesSize:1]; for (int i=0;i<facesSize;i++) faces[i]=tris[i];
    }
    else if (type == (int)MeshType::UVsphere){
        // esfera UV: meshVerts = segments (longitud), meshVerts2 = rings (latitud),
        // meshSize = radio. Cada celda de la grilla = un QUAD (faces3d) -> 1 normal
        // por cara. SMOOTH = grilla compartida con normal radial; FLAT = cada celda
        // con sus propios vertices y la normal de la cara. Las celdas de los polos
        // quedan como quads degenerados (un triangulo) -> 1 normal igual.
        int seg = meshVerts;  if (seg < 3) seg = 3;
        int rin = meshVerts2; if (rin < 2) rin = 2;
        float R = meshSize;
        std::vector<GLfloat> vp, vu; std::vector<GLbyte> vn; std::vector<GLushort> tris;

        if (meshSmooth){
            int cols = seg + 1;
            std::vector<int> grid((rin+1)*cols);
            for (int i=0;i<=rin;i++){
                float lat=(float)M_PI*i/rin, sLat=sinf(lat), cLat=cosf(lat);
                for (int j=0;j<=seg;j++){
                    float lon=2.0f*(float)M_PI*j/seg;
                    float nx=sLat*cosf(lon), ny=cLat, nz=sLat*sinf(lon);
                    grid[i*cols+j]=PushV(vp,vn,vu, R*nx,R*ny,R*nz, nx,ny,nz, (float)j/seg,(float)i/rin);
                }
            }
            for (int i=0;i<rin;i++){
                for (int j=0;j<seg;j++){
                    std::vector<int> ring; // orden hacia afuera (analogo al cono)
                    ring.push_back(grid[(i+1)*cols+j]);
                    ring.push_back(grid[i*cols+j]);
                    ring.push_back(grid[i*cols+j+1]);
                    ring.push_back(grid[(i+1)*cols+j+1]);
                    AddFace(tris, faces3d, ring);
                }
            }
        } else { // FLAT: cada celda con sus 4 vertices y la normal de la cara
            for (int i=0;i<rin;i++){
                float lat0=(float)M_PI*i/rin, lat1=(float)M_PI*(i+1)/rin;
                for (int j=0;j<seg;j++){
                    float lon0=2.0f*(float)M_PI*j/seg, lon1=2.0f*(float)M_PI*(j+1)/seg;
                    // 4 esquinas (mismo orden que el smooth: (i+1,j),(i,j),(i,j+1),(i+1,j+1))
                    float pos[12];
                    float coords[4][2] = { {lat1,lon0},{lat0,lon0},{lat0,lon1},{lat1,lon1} };
                    for (int k=0;k<4;k++){
                        float la=coords[k][0], lo=coords[k][1], sL=sinf(la);
                        pos[k*3]   = R*sL*cosf(lo);
                        pos[k*3+1] = R*cosf(la);
                        pos[k*3+2] = R*sL*sinf(lo);
                    }
                    float fnx,fny,fnz; NewellPos(pos,4,fnx,fny,fnz);
                    std::vector<int> ring;
                    for (int k=0;k<4;k++)
                        ring.push_back(PushV(vp,vn,vu, pos[k*3],pos[k*3+1],pos[k*3+2], fnx,fny,fnz, 0.0f,0.0f));
                    AddFace(tris, faces3d, ring);
                }
            }
        }

        vertexSize = (int)(vp.size()/3);
        vertex = new GLfloat[vp.size()]; for (size_t i=0;i<vp.size();i++) vertex[i]=vp[i];
        normals = new GLbyte[vn.size()]; for (size_t i=0;i<vn.size();i++) normals[i]=vn[i];
        uv = new GLfloat[vu.size()]; for (size_t i=0;i<vu.size();i++) uv[i]=vu[i];
        vertexColor = new GLubyte[vertexSize*4]; for (int i=0;i<vertexSize*4;i++) vertexColor[i]=255;
        facesSize = (int)tris.size();
        faces = new MeshIndex[facesSize>0?facesSize:1]; for (int i=0;i<facesSize;i++) faces[i]=tris[i];
    }
    else if (type == (int)MeshType::cone || type == (int)MeshType::cylinder){
        // cono = cilindro con radio variable: anillo base (n) + anillo top (n) +
        // n caras laterales (quad) + tapa(s) ngon. r2=0 -> el anillo top colapsa
        // al apex (cada vertice top sigue compartido por 2 caras: flat=2 magenta,
        // smooth=1; +1 si hay tapa de arriba). flat = normal por cara; smooth =
        // anillos compartidos con normal de pendiente (las tapas SIEMPRE propias).
        // El CILINDRO es el mismo cono con r2 = r1 (un solo radio).
        int n = meshVerts; if (n < 3) n = 3;
        float r1 = meshSize;
        float r2 = (type == (int)MeshType::cylinder) ? meshSize : meshSize2;
        float hy = meshDepth * 0.5f;
        bool trunc = (r2 > 0.0001f);
        float nyc = r1 - r2; // componente Y de la normal de pendiente
        std::vector<GLfloat> vp, vu; std::vector<GLbyte> vn; std::vector<GLushort> tris;

        if (meshSmooth){
            std::vector<int> baseR(n), topR(n);
            for (int j=0;j<n;j++){
                float a=2.0f*(float)M_PI*j/n, ca=cosf(a), sa=sinf(a);
                float lx=meshDepth*ca, ly=nyc, lz=meshDepth*sa, ln=sqrtf(lx*lx+ly*ly+lz*lz); if(ln<1e-6f)ln=1.0f;
                baseR[j]=PushV(vp,vn,vu, r1*ca,-hy,r1*sa, lx/ln,ly/ln,lz/ln, (float)j/n,0.0f);
            }
            for (int j=0;j<n;j++){
                float a=2.0f*(float)M_PI*j/n, ca=cosf(a), sa=sinf(a);
                float lx=meshDepth*ca, ly=nyc, lz=meshDepth*sa, ln=sqrtf(lx*lx+ly*ly+lz*lz); if(ln<1e-6f)ln=1.0f;
                topR[j]=PushV(vp,vn,vu, r2*ca,hy,r2*sa, lx/ln,ly/ln,lz/ln, (float)j/n,1.0f);
            }
            for (int j=0;j<n;j++){
                int jn=(j+1)%n; std::vector<int> ring;
                ring.push_back(baseR[j]); ring.push_back(topR[j]);
                ring.push_back(topR[jn]); ring.push_back(baseR[jn]);
                AddFace(tris, faces3d, ring);
            }
        } else { // FLAT: cada cara con sus propios vertices y la normal de la cara
            for (int j=0;j<n;j++){
                int jn=(j+1)%n;
                float aj=2.0f*(float)M_PI*j/n, ajn=2.0f*(float)M_PI*jn/n;
                float cj=cosf(aj), sj=sinf(aj), cn=cosf(ajn), sn=sinf(ajn);
                float pos[12];
                pos[0]=r1*cj; pos[1]=-hy; pos[2]=r1*sj;   // base j
                pos[3]=r2*cj; pos[4]= hy; pos[5]=r2*sj;    // top j
                pos[6]=r2*cn; pos[7]= hy; pos[8]=r2*sn;    // top jn
                pos[9]=r1*cn; pos[10]=-hy; pos[11]=r1*sn;  // base jn
                float fnx,fny,fnz; NewellPos(pos,4,fnx,fny,fnz);
                std::vector<int> ring;
                for (int k=0;k<4;k++)
                    ring.push_back(PushV(vp,vn,vu, pos[k*3],pos[k*3+1],pos[k*3+2], fnx,fny,fnz, 0.0f,0.0f));
                AddFace(tris, faces3d, ring);
            }
        }

        // TAPA de abajo: ngon SIN vertice central, normal -Y. Orden FORWARD ->
        // Newell da -Y (y el abanico mira hacia abajo) -> overlay/culling correctos
        {
            std::vector<int> ring;
            for (int j=0;j<n;j++){
                float a=2.0f*(float)M_PI*j/n, ca=cosf(a), sa=sinf(a);
                ring.push_back(PushV(vp,vn,vu, r1*ca,-hy,r1*sa, 0,-1,0, ca*0.5f+0.5f, sa*0.5f+0.5f));
            }
            AddFace(tris, faces3d, ring);
        }
        // TAPA de arriba (solo si truncado): ngon, normal +Y. Orden INVERTIDO -> +Y
        if (trunc){
            std::vector<int> ring;
            for (int j=0;j<n;j++){
                int jr=(n-j)%n; float a=2.0f*(float)M_PI*jr/n, ca=cosf(a), sa=sinf(a);
                ring.push_back(PushV(vp,vn,vu, r2*ca,hy,r2*sa, 0,1,0, ca*0.5f+0.5f, sa*0.5f+0.5f));
            }
            AddFace(tris, faces3d, ring);
        }

        // volcar acumuladores a los arrays de render
        vertexSize = (int)(vp.size()/3);
        vertex = new GLfloat[vp.size()]; for (size_t i=0;i<vp.size();i++) vertex[i]=vp[i];
        normals = new GLbyte[vn.size()]; for (size_t i=0;i<vn.size();i++) normals[i]=vn[i];
        uv = new GLfloat[vu.size()]; for (size_t i=0;i<vu.size();i++) uv[i]=vu[i];
        vertexColor = new GLubyte[vertexSize*4]; for (int i=0;i<vertexSize*4;i++) vertexColor[i]=255;
        facesSize = (int)tris.size();
        faces = new MeshIndex[facesSize>0?facesSize:1]; for (int i=0;i<facesSize;i++) faces[i]=tris[i];
    }
    else { vertexSize = 0; facesSize = 0; } // no es una primitiva conocida

    MaterialGroup g;
    g.startDrawn = 0;
    g.material = mat;
    g.indicesDrawnCount = facesSize;
    materialsGroup.push_back(g);

    CalcularBordes(); // bordes unicos desde faces3d (contorno de seleccion / wireframe)
}

Object* NewMesh(MeshType type, Object* parent, bool query){
    Mesh* mesh = new Mesh(parent, cursor3D.pos);
    if (type == MeshType::plane || type == MeshType::cube ||
        type == MeshType::circle || type == MeshType::UVsphere ||
        type == MeshType::cone || type == MeshType::cylinder){
        mesh->meshTipo = (int)type;
        if (type == MeshType::UVsphere){
            mesh->meshSize = 1.0f;   // radio
            mesh->meshVerts = 16;    // segments
            mesh->meshVerts2 = 8;    // rings
            mesh->meshSmooth = true; // las esferas arrancan suaves
        } else if (type == MeshType::cone){
            mesh->meshSize = 1.0f;   // radio1 (base)
            mesh->meshSize2 = 0.0f;  // radio2 (punta) -> puntiagudo
            mesh->meshDepth = 2.0f;  // altura
            mesh->meshVerts = 8;     // vertices
        } else if (type == MeshType::cylinder){
            mesh->meshSize = 1.0f;   // radio (unico)
            mesh->meshDepth = 2.0f;  // altura
            mesh->meshVerts = 8;     // vertices
        } else {
            mesh->meshSize = (type == MeshType::circle) ? 1.0f : 2.0f; // radio 1 / span 2
            mesh->meshVerts = 8;
        }
        mesh->Regenerar(); // arma vertex/normals/uv/faces + el grupo de material
        mesh->name = (type == MeshType::cube)     ? "Cube"
                   : (type == MeshType::plane)    ? "Plane"
                   : (type == MeshType::UVsphere) ? "UVSphere"
                   : (type == MeshType::cone)     ? "Cone"
                   : (type == MeshType::cylinder) ? "Cylinder" : "Circle";
    } else {
        // vertice / otros: malla vacia con su grupo (como antes)
        MaterialGroup g; g.startDrawn = 0; g.material = MaterialDefecto;
        mesh->materialsGroup.push_back(g);
    }
    return mesh;
};