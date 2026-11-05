#include "animation/Armature2DAnimation.h"
#include "W3dNombres.h"   // LA regla de nombres unicos (compartida)
#include "objects/Mesh.h"
#include "objects/Objects.h"  // SceneCollection: recorrido de la escena en modo juego
#include <stdio.h>   // sprintf GLOBAL (nombre unico del clip): Symbian/STLport no tiene std::snprintf
#include <vector>

// ============================================================================
//  Implementacion de los CLIPS del armature 2D. Clon 1:1 de SkeletalAnimation.cpp acotado a
//  los 5 canales 2D (posU/posV/rot/sclX/sclY). Todo lo que aca parece repetido del 3D lo es a
//  proposito: la alternativa era generalizar BoneTrack sobre un "que se anima" y eso obligaba a
//  tocar el armature 3D (probado y andando) para estrenar el 2D.
// ============================================================================

AnimProperty& Bone2DTrack::PropertyDe(int property, int component) {
    for (size_t i = 0; i < Propertys.size(); i++)
        if (Propertys[i].Property == property && Propertys[i].component == component) return Propertys[i];
    AnimProperty ap;
    ap.Property = property;
    ap.component = component;
    Propertys.push_back(ap);
    return Propertys[Propertys.size() - 1];
}

// version que NO crea (la que usa la evaluacion): PropertyDe ensuciaba el documento con curvas
// vacias con solo mover el playhead.
const AnimProperty* Bone2DTrack::PropertyBuscar(int property, int component) const {
    for (size_t i = 0; i < Propertys.size(); i++)
        if (Propertys[i].Property == property && Propertys[i].component == component) return &Propertys[i];
    return NULL;
}

Bone2DTrack& Armature2DAnimation::TrackDe(int bone) {
    for (size_t i = 0; i < tracks.size(); i++) if (tracks[i].bone == bone) return tracks[i];
    Bone2DTrack t;
    t.bone = bone;
    tracks.push_back(t);
    return tracks[tracks.size() - 1];
}

Armature2DAnimation* Arm2DClipActivo(Mesh* m) {
    if (!m) return NULL;
    if (m->Arm2DAnimActiva() < 0 || m->Arm2DAnimActiva() >= (int)m->Arm2DAnims().size()) return NULL;
    return m->Arm2DAnims()[m->Arm2DAnimActiva()];
}

// nombre libre de clip dentro del armature 2D ACTIVO de la malla
struct Arm2DAnimCtx { Mesh* m; int excepto; };
static bool Arm2DAnimExiste(const std::string& n, void* ctx){
    Arm2DAnimCtx* c = (Arm2DAnimCtx*)ctx;
    const std::vector<Armature2DAnimation*>& v = c->m->Arm2DAnims();
    for (size_t i = 0; i < v.size(); i++)
        if ((int)i != c->excepto && v[i] && v[i]->name == n) return true;
    return false;
}
std::string Arm2DAnimNombreLibre(Mesh* m, const std::string& base, int excepto){
    if (!m) return base;
    Arm2DAnimCtx c; c.m = m; c.excepto = excepto;
    return W3dNombreUnico(base, "Anim2D", Arm2DAnimExiste, &c);
}

void Arm2DCrearAnimacion(Mesh* m) {
    if (!m || !m->TieneArm2D()) return;   // los clips viven DENTRO de un armature 2D
    // antes: sprintf("Anim2D.%03d", size()+1) SIN chequear -> borrar un clip del medio y
    // crear otro repetia nombre. Ahora pasa por LA regla comun.
    Armature2DAnimation* an = new Armature2DAnimation(Arm2DAnimNombreLibre(m, "Anim2D", -1));
    an->fps = AnimFPS > 0 ? AnimFPS : 30;
    an->startFrame = 1;
    an->endFrame = 0;                // vacio: se extiende al insertar el primer keyframe
    m->Arm2DAnims().push_back(an);
    m->Arm2DAnimActiva() = (int)m->Arm2DAnims().size() - 1;
    m->pose2dDirty = true;
}

void Arm2DBorrarAnimacionActiva(Mesh* m) {
    if (!m) return;
    int i = m->Arm2DAnimActiva();
    if (i < 0 || i >= (int)m->Arm2DAnims().size()) return;
    delete m->Arm2DAnims()[i];
    m->Arm2DAnims().erase(m->Arm2DAnims().begin() + i);
    if (m->Arm2DAnimActiva() >= (int)m->Arm2DAnims().size()) m->Arm2DAnimActiva() = (int)m->Arm2DAnims().size() - 1;
    m->last2dAnim = -999; m->pose2dDirty = true;
}

// ---------------------------------------------------------------------------
//  ARMATURE ACTIVO de la malla + el rango del timeline (ver el .h)
// ---------------------------------------------------------------------------
void Arm2DSincronizarRango(Mesh* m) {
    if (!m) return;
    if (ActiveAnimKind != 4 || ActiveAnimMesh != m) return; // el timeline no muestra un clip 2D de esta malla
    if (!Arm2DClipActivo(m)) return;   // el rig recien activado no tiene clips: el timeline se queda como
                                       // esta (recargar caeria al rango de la ESCENA, que no es lo que muestra)
    AnimCargarRangoActivo();           // Start/End/FPS <- el clip del armature que quedo activo
    m->last2dFrame = -999999; m->last2dAnim = -999; // otro clip manda: re-evaluar la pose
}

void Arm2DSetActivo(Mesh* m, int idx) {
    if (!m || idx < 0 || idx >= (int)m->armatures2d.size()) return;
    if (m->armature2dActivo == idx) return;
    m->armature2dActivo = idx;
    Arm2DSincronizarRango(m);
}

// clon PROFUNDO (huesos + clips). Los clips son punteros de los que el armature es DUEÑO -> uno
// nuevo por cada uno (el contenido de Armature2DAnimation si es copiable por valor: tracks/curvas
// son vectores). Mismo criterio que la copia de las anims del armature 3D / las vertex anims.
Armature2D* Arm2DClonar(const Armature2D* src) {
    if (!src) return NULL;
    Armature2D* d = new Armature2D(src->nombre);
    d->huesos     = src->huesos;      // W3dBone2D es POD-ish: copia por valor
    d->animActiva = src->animActiva;
    d->boneActivo = src->boneActivo;
    for (size_t i = 0; i < src->anims.size(); i++) {
        const Armature2DAnimation* a = src->anims[i];
        if (!a) { d->anims.push_back(NULL); continue; }   // hueco: se respeta la posicion (animActiva indexa)
        Armature2DAnimation* c = new Armature2DAnimation(a->name);
        c->fps = a->fps; c->startFrame = a->startFrame; c->endFrame = a->endFrame;
        c->tracks = a->tracks;         // copia profunda (vectores por valor, como Arm2DDuplicarAnimacionActiva)
        d->anims.push_back(c);
    }
    return d;
}

void Arm2DDuplicarAnimacionActiva(Mesh* m) {
    Armature2DAnimation* src = Arm2DClipActivo(m);
    if (!src) return;
    // el ".copy" era OTRA convencion (y sin chequeo: duplicar dos veces repetia)
    Armature2DAnimation* dup = new Armature2DAnimation(Arm2DAnimNombreLibre(m, src->name, -1));
    dup->fps = src->fps; dup->startFrame = src->startFrame; dup->endFrame = src->endFrame;
    dup->tracks = src->tracks;       // copia profunda (vectores por valor)
    m->Arm2DAnims().push_back(dup);
    m->Arm2DAnimActiva() = (int)m->Arm2DAnims().size() - 1;
    m->pose2dDirty = true;
}

// ---------------------------------------------------------------------------
//  EVALUACION
// ---------------------------------------------------------------------------
// que clip reproduce CADA armature 2D de la malla (-1 = ninguno) + una FIRMA de esa eleccion
// (reemplaza al viejo 'animPlay' suelto en el cache last2dAnim: ahora hay una eleccion por
// armature y el cache tiene que invalidarse si cambia cualquiera).
// GATING (mismo criterio que EvaluarPoseEsqueleto): un rig 2D reproduce su clip si es la
// animacion ACTIVA del timeline (kind 4 + esta malla + es el armature ACTIVO -- el timeline
// muestra UN clip por vez)... o si estamos en MODO JUEGO (kind 2), donde no hay "animacion
// activa" y TODOS los armatures de TODAS las mallas tienen que animarse (el camino viejo
// horneado en la capa uv de la vertex anim si corria en el juego).
// Con una escena o con otra cosa activa se queda en la pose que tenga (la que va al .w3d).
static int Arm2DPlanDeJuego(Mesh* m, std::vector<int>& play) {
    const bool juego = (ActiveAnimKind == 2);
    play.assign(m->armatures2d.size(), -1);
    int sig = 0;
    for (size_t a = 0; a < m->armatures2d.size(); a++) {
        Armature2D* arm = m->armatures2d[a];
        if (arm) {
            const bool suyo = juego || (ActiveAnimKind == 4 && ActiveAnimMesh == m &&
                                        (int)a == m->armature2dActivo);
            if (suyo && arm->animActiva >= 0 && arm->animActiva < (int)arm->anims.size())
                play[a] = arm->animActiva;
        }
        sig = sig * 131 + (play[a] + 1);
    }
    return sig;
}

// escribe la pose de los huesos de UN armature desde las curvas de su clip
static void Arm2DEvaluarUno(Armature2D* arm, Armature2DAnimation* clip, int frame) {
    for (size_t i = 0; i < arm->huesos.size(); i++) {
        W3dBone2D& b = arm->huesos[i];
        Bone2DTrack* tr = NULL;
        for (size_t t = 0; t < clip->tracks.size(); t++)
            if (clip->tracks[t].bone == (int)i) { tr = &clip->tracks[t]; break; }
        if (!tr) continue;   // hueso sin track: queda como esta (no se fuerza identidad)
        // cada componente tiene SU curva: el canal sin keyframes propios queda en su valor de
        // IDENTIDAD (traslacion 0, rotacion 0, escala 1) = el equivalente 2D de la rest del 3D.
        // PropertyBuscar y NO PropertyDe: evaluar NO puede crear curvas (mover el playhead
        // ensuciaba el documento con hasta 5 AnimProperty vacias por hueso).
        const AnimProperty* p;
        p = tr->PropertyBuscar(AnimPosition, AnimX); b.poseTU  = p ? p->Eval(frame, 0.0f) : 0.0f;
        p = tr->PropertyBuscar(AnimPosition, AnimY); b.poseTV  = p ? p->Eval(frame, 0.0f) : 0.0f;
        p = tr->PropertyBuscar(AnimRotation, AnimX); b.poseRot = p ? p->Eval(frame, 0.0f) : 0.0f;
        p = tr->PropertyBuscar(AnimScale,    AnimX); b.poseSX  = p ? p->Eval(frame, 1.0f) : 1.0f;
        p = tr->PropertyBuscar(AnimScale,    AnimY); b.poseSY  = p ? p->Eval(frame, 1.0f) : 1.0f;
    }
}

void Armature2DEvaluar(Mesh* m, int frame) {
    if (!m || !m->TieneArm2D()) return;
    std::vector<int> play;
    const int sig = Arm2DPlanDeJuego(m, play);
    bool frameChanged = (m->last2dFrame != frame || m->last2dAnim != sig);
    if (!frameChanged && !m->pose2dDirty) return;
    m->last2dFrame = frame; m->last2dAnim = sig;
    m->pose2dDirty = false;
    bool alguno = false;
    for (size_t a = 0; a < play.size(); a++) {
        if (play[a] < 0) continue;   // armature sin clip activo: la pose a mano manda (no se pisa)
        alguno = true;
        if (frameChanged) Arm2DEvaluarUno(m->armatures2d[a], m->armatures2d[a]->anims[play[a]], frame);
    }
    if (!alguno) return;
    m->Armature2DRestCapturar();
    m->Armature2DAplicar();
    m->skinGeomVersion++;        // el uv cambio -> re-subir el VBO
}

// MODO JUEGO: no hay una malla "activa" del timeline, asi que se evalua TODA la escena. Se llama
// desde el loop del editor con el Play puesto (main/app/main.cpp). El runtime standalone
// (Whisk3D-Examples/game/w3drun.cpp) es UI 2D pura -stubbea Mesh- y no tiene mallas que evaluar:
// si algun dia el runtime carga escenas 3D, este es el llamado que le falta.
void Armature2DEvaluarEscena(int frame) {
    if (!SceneCollection) return;
    std::vector<Object*> pila;
    pila.push_back(SceneCollection);
    while (!pila.empty()) {
        Object* o = pila[pila.size() - 1]; pila.pop_back();
        if (o->getType() == ObjectType::mesh) {
            Mesh* me = (Mesh*)o;
            bool hayClips = false;   // CUALQUIER armature 2D de la malla con clips (no solo el activo)
            for (size_t a = 0; a < me->armatures2d.size() && !hayClips; a++)
                if (me->armatures2d[a] && !me->armatures2d[a]->anims.empty() &&
                    !me->armatures2d[a]->huesos.empty()) hayClips = true;
            if (hayClips) Armature2DEvaluar(me, frame);
        }
        for (size_t i = 0; i < o->Childrens.size(); i++) pila.push_back(o->Childrens[i]);
    }
}

// ---------------------------------------------------------------------------
//  INSERT KEYFRAME
// ---------------------------------------------------------------------------
// pone (o actualiza) el keyframe de UN canal
static void SetKey1(Bone2DTrack& tr, int prop, int comp, int frame, float v) {
    SetKeyCurva(tr.PropertyDe(prop, comp), frame, v, HPlano);
}

void InsertarKeyframeArm2D(Mesh* m, int canales) {
    if (!m || m->Arm2DHuesos().empty()) return;
    if (canales == 0) canales = KfCanalTodos;
    if (m->Arm2DAnimActiva() < 0 || m->Arm2DAnimActiva() >= (int)m->Arm2DAnims().size()) Arm2DCrearAnimacion(m);
    Armature2DAnimation* clip = Arm2DClipActivo(m);
    if (!clip) return;
    int nSel = 0;
    for (size_t i = 0; i < m->Arm2DHuesos().size(); i++) {
        W3dBone2D& b = m->Arm2DHuesos()[i];
        // los huesos SELECCIONADOS y ademas el ACTIVO (aunque no este seleccionado), igual que el 3D
        if (!b.select && (int)i != m->Arm2DBoneActivo()) continue;
        Bone2DTrack& tr = clip->TrackDe((int)i);
        if (canales & KfCanalLoc) { SetKey1(tr, AnimPosition, AnimX, CurrentFrame, b.poseTU);
                                    SetKey1(tr, AnimPosition, AnimY, CurrentFrame, b.poseTV); }
        if (canales & KfCanalRot)   SetKey1(tr, AnimRotation, AnimX, CurrentFrame, b.poseRot);
        if (canales & KfCanalScl) { SetKey1(tr, AnimScale,    AnimX, CurrentFrame, b.poseSX);
                                    SetKey1(tr, AnimScale,    AnimY, CurrentFrame, b.poseSY); }
        nSel++;
    }
    if (nSel == 0) return;
    if (CurrentFrame > clip->endFrame) clip->endFrame = CurrentFrame;
    if (CurrentFrame < clip->startFrame || clip->startFrame < 1) clip->startFrame = CurrentFrame < 1 ? 1 : CurrentFrame;
    // NO forzar re-lectura de la curva (misma trampa que InsertarKeyframeEsqueleto, bug ya
    // reportado en el 3D): refrescar desde el clip devolveria a IDENTIDAD los huesos que todavia
    // no tienen keyframe, y se perderia la pose que el usuario hizo a mano en los demas.
    std::vector<int> play;
    m->last2dFrame = CurrentFrame; m->last2dAnim = Arm2DPlanDeJuego(m, play); m->pose2dDirty = false;
}

// ---------------------------------------------------------------------------
//  AUTO KEY (por canal, solo lo que cambio)
// ---------------------------------------------------------------------------
bool AutoKeyArm2DPrep(Mesh* m) {
    if (!m || m->Arm2DHuesos().empty()) return false;
    if (m->Arm2DAnimActiva() < 0 || m->Arm2DAnimActiva() >= (int)m->Arm2DAnims().size()) Arm2DCrearAnimacion(m);
    return Arm2DClipActivo(m) != NULL;
}

int AutoKeyHueso2D(Mesh* m, int i, float tu0, float tv0, float rot0, float sx0, float sy0) {
    Armature2DAnimation* clip = Arm2DClipActivo(m);
    if (!clip || !m || i < 0 || i >= (int)m->Arm2DHuesos().size()) return 0;
    W3dBone2D& b = m->Arm2DHuesos()[i];
    Bone2DTrack& tr = clip->TrackDe(i);
    // tolerancia RELATIVA (misma que AutoKeyHueso del 3D): la pose pasa por trigonometria y un
    // canal que no se toco vuelve con basura en el ultimo bit -> con == se guardaria todo siempre.
    struct C { static bool cambio(float x, float y){
        float d = x-y; if (d<0) d=-d;
        float ma = (x<0?-x:x), mb = (y<0?-y:y); if (mb>ma) ma=mb;
        return d > 1e-5f * (1.0f + ma); } };
    const int props[5] = { AnimPosition, AnimPosition, AnimRotation, AnimScale, AnimScale };
    const int comps[5] = { AnimX,        AnimY,        AnimX,        AnimX,     AnimY     };
    const float nuevo[5] = { b.poseTU, b.poseTV, b.poseRot, b.poseSX, b.poseSY };
    const float viejo[5] = { tu0,      tv0,      rot0,      sx0,      sy0      };
    int n = 0;
    for (int c = 0; c < 5; c++) {
        if (!C::cambio(nuevo[c], viejo[c])) continue;
        SetKeyCurva(tr.PropertyDe(props[c], comps[c]), CurrentFrame, nuevo[c], HPlano);
        n++;
    }
    return n;
}

void AutoKeyArm2DFin(Mesh* m) {
    Armature2DAnimation* clip = Arm2DClipActivo(m);
    if (!clip) return;
    if (CurrentFrame > clip->endFrame) clip->endFrame = CurrentFrame;
    if (CurrentFrame < clip->startFrame || clip->startFrame < 1) clip->startFrame = CurrentFrame < 1 ? 1 : CurrentFrame;
    std::vector<int> play;
    m->last2dFrame = CurrentFrame; m->last2dAnim = Arm2DPlanDeJuego(m, play); m->pose2dDirty = false;
}
