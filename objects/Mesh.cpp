#include "Mesh.h"
#include "w3dGraphics.h"    // abstraccion de graficos del engine (sin GL)
#include "CameraBase.h"     // g_renderCamPos (camara del render, para el chrome equirect)
#include "RenderColors.h"   // paleta de render del CORE (sin depender de la UI)
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
    genChromeExpPos = NULL; genChromeExpUV = NULL; genChromeCount = 0; genChromeValid = false; // reflejo de la malla generada (lazy)
    tangents = NULL; nmColors = NULL; tangentsValid = false; // normal mapping (lazy)
}

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

// CHROME sobre la MALLA GENERADA (screw/subdiv): mismas UV de reflejo que ActualizarChromeUV pero leyendo la geo
// GENERADA (genVertex/genNormals/genFaces), y SOLO para los mesh parts con reflejo. Si ningun part tiene chrome NO
// hace NADA (costo 0, no recalcula frame a frame). Cache por camara/mundo -> solo "paga" al orbitar. NO toca la geo 3D.
void Mesh::ActualizarChromeUVGen() {
    if (!genValido || !genVertex || !genNormals || !genFaces || genFacesSize <= 0) return;
    // hay algun mesh part con reflejo? (material LIVE de materialsGroup, mismo indice de grupo)
    bool hayChrome = false;
    for (size_t g = 0; g < genMaterialsGroup.size(); g++) {
        Material* mat = (g < materialsGroup.size()) ? materialsGroup[g].material : genMaterialsGroup[g].material;
        if (mat && mat->chrome) { hayChrome = true; break; }
    }
    if (!hayChrome) return; // sin reflejo: no se recalcula nada
    Matrix4 W; GetWorldMatrix(W);
    Vector3 cam = g_renderCamPos;
    Vector3 cr = g_renderCamRight, cu = g_renderCamUp, cf = g_renderCamForward;
    // cache hit: misma camara + mismo mundo + misma cantidad de corners -> nada que hacer
    if (genChromeValid && genChromeExpUV && genChromeCount == genFacesSize) {
        bool igual = (genChromeCam.x == cam.x && genChromeCam.y == cam.y && genChromeCam.z == cam.z);
        for (int i = 0; igual && i < 16; i++) if (genChromeW[i] != W.m[i]) igual = false;
        if (igual) return;
    }
    if (!genChromeExpUV || genChromeCount != genFacesSize) {
        delete[] genChromeExpPos; genChromeExpPos = new GLfloat[genFacesSize * 3];
        delete[] genChromeExpUV;  genChromeExpUV  = new GLfloat[genFacesSize * 2];
        genChromeCount = genFacesSize;
    }
    const float PI = 3.14159265358979f;
    // por cada GRUPO con chrome, computa las UV de reflejo de SUS corners (los demas quedan sin tocar).
    for (size_t g = 0; g < genMaterialsGroup.size(); g++) {
        Material* mat = (g < materialsGroup.size()) ? materialsGroup[g].material : genMaterialsGroup[g].material;
        if (!mat || !mat->chrome) continue;
        bool equirect = (mat->reflectMode == 2);
        int s = genMaterialsGroup[g].startDrawn, cnt = genMaterialsGroup[g].indicesDrawnCount;
        for (int t = s; t + 2 < s + cnt; t += 3) {
            float u[3], v[3]; bool polo[3]; Vector3 lp[3];
            for (int k = 0; k < 3; k++) {
                int vi = genFaces[t + k];
                lp[k] = Vector3(genVertex[vi*3], genVertex[vi*3+1], genVertex[vi*3+2]);
                Vector3 ln(genNormals[vi*3]/127.0f, genNormals[vi*3+1]/127.0f, genNormals[vi*3+2]/127.0f);
                Vector3 wp = W * lp[k];
                Vector3 wn(W.m[0]*ln.x + W.m[4]*ln.y + W.m[8]*ln.z,
                           W.m[1]*ln.x + W.m[5]*ln.y + W.m[9]*ln.z,
                           W.m[2]*ln.x + W.m[6]*ln.y + W.m[10]*ln.z);
                wn = wn.Normalized();
                polo[k] = false;
                if (equirect) {
                    Vector3 I = (wp - cam).Normalized();
                    float dd = I.x*wn.x + I.y*wn.y + I.z*wn.z;
                    Vector3 R(I.x - 2*dd*wn.x, I.y - 2*dd*wn.y, I.z - 2*dd*wn.z);
                    float ry = R.y < -1.0f ? -1.0f : (R.y > 1.0f ? 1.0f : R.y);
                    u[k] = atan2f(R.z, R.x) / (2.0f*PI) + 0.5f;
                    v[k] = acosf(ry) / PI;
                    polo[k] = (ry > 0.995f || ry < -0.995f);
                } else {
                    Vector3 rel = wp - cam;
                    Vector3 ep(rel.x*cr.x + rel.y*cr.y + rel.z*cr.z,
                               rel.x*cu.x + rel.y*cu.y + rel.z*cu.z,
                             -(rel.x*cf.x + rel.y*cf.y + rel.z*cf.z));
                    Vector3 en(wn.x*cr.x + wn.y*cr.y + wn.z*cr.z,
                               wn.x*cu.x + wn.y*cu.y + wn.z*cu.z,
                             -(wn.x*cf.x + wn.y*cf.y + wn.z*cf.z));
                    Vector3 I = ep.Normalized();
                    float dd = I.x*en.x + I.y*en.y + I.z*en.z;
                    Vector3 R(I.x - 2*dd*en.x, I.y - 2*dd*en.y, I.z - 2*dd*en.z);
                    float mm = 2.0f * sqrtf(R.x*R.x + R.y*R.y + (R.z+1.0f)*(R.z+1.0f));
                    if (mm < 1e-5f) mm = 1e-5f;
                    u[k] = R.x / mm + 0.5f;
                    v[k] = 1.0f - (R.y / mm + 0.5f);
                }
            }
            if (equirect) {
                float umin = 2.0f, umax = -1.0f;
                for (int k = 0; k < 3; k++) if (!polo[k]) { if (u[k] < umin) umin = u[k]; if (u[k] > umax) umax = u[k]; }
                if (umax > umin && umax - umin > 0.5f)
                    for (int k = 0; k < 3; k++) if (!polo[k] && u[k] < 0.5f) u[k] += 1.0f;
                float sum = 0.0f; int n = 0;
                for (int k = 0; k < 3; k++) if (!polo[k]) { sum += u[k]; n++; }
                if (n > 0) { float avg = sum / n; for (int k = 0; k < 3; k++) if (polo[k]) u[k] = avg; }
            }
            for (int k = 0; k < 3; k++) {
                int c = t + k;
                genChromeExpPos[c*3] = (GLfloat)lp[k].x; genChromeExpPos[c*3+1] = (GLfloat)lp[k].y; genChromeExpPos[c*3+2] = (GLfloat)lp[k].z;
                genChromeExpUV[c*2] = u[k]; genChromeExpUV[c*2+1] = v[k];
            }
        }
    }
    genChromeCam = cam;
    for (int i = 0; i < 16; i++) genChromeW[i] = W.m[i];
    genChromeValid = true;
}

// ===================================================
// aplica TODO el estado GL de un material, leyendolo DEL material (nada
// hardcodeado). RenderObject la llama solo cuando el material cambia.
void Mesh::AplicarMaterial(Material* mat, bool conLuz, bool solido) {
    namespace gfx = w3dEngine;
    gfx::SmoothShading(true); // el look suave/plano lo dan las NORMALES de la malla, no el material
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
        // MATCAP (mode 0): por HARDWARE solo si hay matriz de textura (PC/N95). En GLES2 (Android/WebGL) esa
        // matriz es un no-op -> el matcap va por SOFTWARE, igual que el equirect. Sino no se veia (pedido Dante).
        bool matcap     = mat->chrome && mat->reflectMode == 0 && gfx::TieneTexGen();  // matcap HW
        bool matcapSW   = mat->chrome && mat->reflectMode == 0 && !gfx::TieneTexGen(); // matcap SOFTWARE (GLES2)
        bool sphereExact= mat->chrome && mat->reflectMode == 1;
        bool eq         = mat->chrome && mat->reflectMode == 2;
        bool sphereHW   = sphereExact && gfx::TieneTexGen();      // PC: sphere exacto por texgen
        bool sw         = eq || (sphereExact && !gfx::TieneTexGen()) || matcapSW; // SOFTWARE: equirect + sphere/matcap sin HW
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
    // sin vertices no hay nada. Sin CARAS propias igual hay que dibujar: en Edit (edit mesh: verts+bordes), o si
    // hay malla GENERADA por modificadores (ej. Screw sobre un perfil sin caras -> la botella vive en genValido),
    // o si hay loose edges (wireframe suelto que se ve como overlay). Sino en modo objeto una malla sin caras
    // propias quedaba INVISIBLE aunque tuviera geometria generada/suelta -> "no se ve la botella".
    if (!vertex || vertexSize <= 0) return;
    const bool hayGen = (genValido && genVertex && genFaces);
    if ((!faces || facesSize < 3) && !editActiva && !hayGen && looseEdges.empty()) return;
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

    // PASES PLANOS (Normal View / ZBuffer / Alpha): la malla se dibuja UNLIT con un COLOR PLANO y, para
    // los materiales TRANSPARENTES, se usa SOLO el alpha de su textura (el COLOR de la textura NO se
    // muestra) via TexEnvAlphaOnly. El editor elige el modo con estos flags; el Core solo sabe "dibujar
    // plano con alpha-only". NO aplica luz, color de textura, chrome, normal map ni capas -> liviano (N95).
    //   - Normal View (w3dRenderNormalColor): color = normal en VIEW-SPACE por vertice (se recalcula por
    //     frame -> "gira" con la camara, util para composicion). Malla BASE.
    //   - ZBuffer / Alpha (w3dRenderSinLuz / w3dRenderAlpha): color = BLANCO. El editor le pone fog al
    //     ZBuffer (degrade de profundidad); el Alpha va SIN fog = matte de cobertura. Soporta malla generada.
    {
    const bool paseNormal = w3dRenderNormalColor;
    const bool paseBlanco = w3dRenderSinLuz || w3dRenderAlpha;
    if ((paseNormal || paseBlanco) && faces && facesSize >= 3) {
        gfx::Disable(gfx::Lighting);
        gfx::DisableArray(gfx::NormalArray);
        gfx::Enable(gfx::DepthTest);

        // normal usa la malla BASE (color por vertice tiene que matchear la geometria); el blanco puede
        // usar la malla GENERADA por modificadores (color uniforme -> sin problema de indices).
        const bool useGen = paseBlanco && (genValido && genVertex && genFaces);
        gfx::VertexPointer3f(0, useGen ? genVertex : vertex);

        if (paseNormal && normals) {
            static std::vector<unsigned char> nvcol; // reusado entre frames (sin alloc)
            nvcol.resize((size_t)vertexSize * 4);
            Matrix4 W; GetWorldMatrix(W);
            Vector3 cr = g_renderCamRight, cu = g_renderCamUp, cf = g_renderCamForward;
            for (int i = 0; i < vertexSize; i++) {
                float lx = normals[i*3+0]/127.0f, ly = normals[i*3+1]/127.0f, lz = normals[i*3+2]/127.0f;
                Vector3 wn(W.m[0]*lx + W.m[4]*ly + W.m[8]*lz,   // object-space -> mundo (sin traslacion)
                           W.m[1]*lx + W.m[5]*ly + W.m[9]*lz,
                           W.m[2]*lx + W.m[6]*ly + W.m[10]*lz);
                wn = wn.Normalized();
                float ex = wn.Dot(cr), ey = wn.Dot(cu), ez = -wn.Dot(cf); // eye-space (-forward = +Z)
                nvcol[i*4+0] = (unsigned char)((ex*0.5f + 0.5f) * 255.0f); // -1..1 -> 0..255
                nvcol[i*4+1] = (unsigned char)((ey*0.5f + 0.5f) * 255.0f);
                nvcol[i*4+2] = (unsigned char)((ez*0.5f + 0.5f) * 255.0f);
                nvcol[i*4+3] = 255;
            }
            gfx::EnableArray(gfx::ColorArray); gfx::ColorPointer4ub(&nvcol[0]);
        } else {
            gfx::DisableArray(gfx::ColorArray);
            gfx::Color4f(1.0f, 1.0f, 1.0f, 1.0f); // ZBuffer / Alpha = BLANCO
        }

        // POR GRUPO de material: transparente -> textura con SOLO alpha (blend); opaco -> color plano solido
        size_t ng = useGen ? genMaterialsGroup.size() : materialsGroup.size();
        for (size_t g = 0; g < ng; g++) {
            const MaterialGroup& grp = useGen ? genMaterialsGroup[g] : materialsGroup[g];
            Material* mat = grp.material ? grp.material : MaterialDefecto;
            bool trans = mat->transparent && mat->texture && mat->textureOn && (useGen ? (genUV != 0) : (uv != 0));
            if (trans) {
                gfx::Enable(gfx::Texture2D);
                gfx::BindTexture(mat->texture->iID);
                gfx::TexFilter(mat->filtrado); gfx::TexWrap(mat->repeat);
                gfx::EnableArray(gfx::TexCoordArray);
                gfx::TexCoordPointer2f(0, useGen ? genUV : uv);
                gfx::TexEnvAlphaOnly(true);          // RGB = color plano, ALPHA = alpha de la textura
                gfx::Enable(gfx::Blend); gfx::BlendAlpha();
            } else {
                gfx::Disable(gfx::Texture2D);
                gfx::TexEnvAlphaOnly(false);
                gfx::DisableArray(gfx::TexCoordArray);
                gfx::Disable(gfx::Blend);            // opaco: color plano solido (rapido)
            }
            if (mat->culling) gfx::Enable(gfx::CullFace); else gfx::Disable(gfx::CullFace);
            if (useGen) gfx::DrawTriangles(grp.indicesDrawnCount, &genFaces[grp.startDrawn]);
            else        gfx::DrawTriangles(grp.indicesDrawnCount, &faces[grp.startDrawn]);
        }

        // limpieza: que el estado del pase plano no leakee al proximo objeto / overlay
        gfx::TexEnvAlphaOnly(false);
        gfx::Disable(gfx::Texture2D);
        gfx::Disable(gfx::Blend);
        gfx::DisableArray(gfx::ColorArray);
        gfx::DisableArray(gfx::TexCoordArray);
        if (useGen) gfx::VertexPointer3f(0, vertex); // restaura las posiciones base
        return;
    }
    }

    // WIREFRAME: usa los BORDES precalculados (mas barato que el wireframe de
    // triangulos). Verde si esta seleccionada, gris si no. Sin bordes -> fallback.
    if (w3dRenderWireframe) {
        if (editActiva && w3dRenderOverlays) {
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
        const bool solido = w3dRenderSolido;
        const bool conLuz = !w3dRenderSinLuz;
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
            // El material es un PUNTERO. genMaterialsGroup es un SNAPSHOT (copia) tomado al GENERAR la malla: si
            // despues se reasigna el material del mesh part, ese snapshot apunta al material VIEJO y el cambio no se
            // ve hasta regenerar (entrar a Edit Mode, mover un vert...). Para que sea responsive, el material se toma
            // LIVE de materialsGroup[g] (mismo indice de grupo; gen copia el orden). Solo cambia el material, no la geo.
            Material* mat = (useGen && g < materialsGroup.size()) ? materialsGroup[g].material : grp.material;
            if (solido || !mat) mat = MaterialDefecto;
            if (mat != ultimo) { AplicarMaterial(mat, conLuz, solido); ultimo = mat; }
            if (useGen) { // malla generada
                // REFLEJO por SOFTWARE (equirect mode 2, o sphere-sw sin texgen): dibuja los corners NO-INDEXADOS
                // con las UV de reflejo calculadas desde la GEO GENERADA. Asi el reflejo se ve sin "aplicar" el
                // modificador. Solo se calcula para los mesh parts con chrome (ActualizarChromeUVGen lo filtra + cachea).
                // matcap (mode 0) por SOFTWARE cuando no hay HW (GLES2) -> junto al equirect/sphere-sw. El matcap
                // HW (matriz de textura) solo existe en PC/N95 (TieneTexGen).
                bool chromeSW = !solido && mat->chrome && (mat->reflectMode == 2 || (mat->reflectMode == 1 && !gfx::TieneTexGen()) || (mat->reflectMode == 0 && !gfx::TieneTexGen()));
                bool matcapHW = !solido && mat->chrome && mat->reflectMode == 0 && gfx::TieneTexGen();
                bool sphereHW = !solido && mat->chrome && mat->reflectMode == 1 && gfx::TieneTexGen();
                if (chromeSW && genNormals) {
                    ActualizarChromeUVGen();
                    if (genChromeExpPos && genChromeExpUV && genChromeCount == genFacesSize) {
                        gfx::VertexPointer3f(0, genChromeExpPos);
                        gfx::EnableArray(gfx::TexCoordArray); gfx::TexCoordPointer2f(0, genChromeExpUV);
                        gfx::DrawTrianglesArrayFrom(grp.startDrawn, grp.indicesDrawnCount);
                        gfx::VertexPointer3f(0, genVertex); // re-bindea las posiciones gen para el proximo grupo
                        ultimo = NULL;                      // cambiamos punteros -> re-AplicarMaterial el proximo
                        continue;
                    }
                }
                if (matcapHW && genNormals) { // matcap HW (solo PC/N95): normales como texcoords (matriz de textura ya puesta)
                    gfx::EnableArray(gfx::TexCoordArray); gfx::TexCoordPointer3b(genNormals);
                    gfx::DrawTriangles(grp.indicesDrawnCount, &genFaces[grp.startDrawn]);
                    ultimo = NULL; // cambiamos el puntero de texcoords -> re-AplicarMaterial el proximo
                    continue;
                }
                // sphere HW (el texgen genera las texcoords solo) o NO-chrome: draw indexado. AplicarMaterial
                // re-bindea el TexCoordPointer al uv BASE -> volvemos a genUV (sino la textura del Screw/Subdiv sale
                // con las UV del perfil, mal indexadas). Con texgen (sphereHW) NO forzamos genUV.
                if (!sphereHW && genUV) { gfx::EnableArray(gfx::TexCoordArray); gfx::TexCoordPointer2f(0, genUV); }
                gfx::DrawTriangles(grp.indicesDrawnCount, &genFaces[grp.startDrawn]);
                continue;
            }
            // REFLECTION por SOFTWARE (equirect SIEMPRE, o sphere exacto en GLES1/N95): render NO INDEXADO por-corner.
            // El MATCAP (matriz de textura) y el sphere HW van por el draw INDEXADO normal (else) -> no entran aca.
            if (!solido && mat->chrome && (mat->reflectMode == 2 || (mat->reflectMode == 1 && !gfx::TieneTexGen()) || (mat->reflectMode == 0 && !gfx::TieneTexGen())) && chromeExpPos && chromeExpUV) {
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





// dibuja el buffer de bordes PRECALCULADO (bordesBuf) como GL_LINES. No arma nada
// por frame: solo color + DrawLines. Lo usan el contorno de seleccion y el wireframe.
void Mesh::RenderBordes(const float* color, float width, bool pushBack) {
    namespace gfx = w3dEngine;
    // con malla GENERADA por modificadores (subdiv/screw) el contorno usa SUS aristas de poligono; sino la
    // base (bordesBuf). Sin esto, un objeto con modificador no mostraba borde de seleccion (la base no coincide).
    const bool usaGen = (genValido && !genBordesBuf.empty() && genVertex);
    const std::vector<GLfloat>& buf = usaGen ? genBordesBuf : bordesBuf;
    GLfloat* restore = usaGen ? genVertex : vertex;
    if (buf.empty() || !restore) return;
    gfx::Disable(gfx::Lighting);
    gfx::Disable(gfx::Texture2D);
    gfx::DisableArray(gfx::NormalArray);
    gfx::DisableArray(gfx::ColorArray);
    gfx::DisableArray(gfx::TexCoordArray);
    gfx::Color4f(color[0], color[1], color[2], 1.0f);
    gfx::LineWidth(width);
    if (pushBack) gfx::DepthRange(0.0008f, 1.0f); // (el contorno usa glPolygonOffset)

    gfx::VertexPointer3f(0, &buf[0]);
    gfx::DrawLines((int)(buf.size()/3));

    if (pushBack) gfx::DepthRange(0.0f, 1.0f);
    gfx::LineWidth(1.0f);
    gfx::VertexPointer3f(0, restore);
    gfx::Invalidate();
}
