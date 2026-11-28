// ============================================================================
//  w3drun.cpp — ver w3drun.h. El runtime del juego 2D + los globals y stubs
//  que las piezas reusadas del editor esperan (asi el juego linkea sin arrastrar
//  todo el editor).
// ============================================================================
#include "w3drun.h"
#include "objects/Objects.h"
#include "WhiskUI/objects/UI.h"
#include "WhiskUI/objects/Text2D.h"
#include "WhiskUI/objects/Image2D.h"
#include "WhiskUI/objects/Element2D.h"
#include "WhiskUI/io/UI2DFormat.h"
#include "WhiskUI/render/UIOverlay.h"
#include "script/W3dScript.h"
#include "script/BindsGame.h"     // los binds 2D de juego COMPARTIDOS con el editor (Play)
#include "physics/W3dPhysics.h"   // paso de fisica del Core (el MISMO que corre el Play del editor)
#include "physics/W3dRigid.h"     // cuerpos rigidos del Core (idem: el MISMO paso que TickReal)
#include "physics/W3dHitbox.h"    // hitbox + contactos -> alEntrar/alQuedarse/alSalir/alTocar
#include "W3dScene.h"             // motor multi-escena COMPARTIDO con el editor (activa / changeScene)
#include "w3dGraphics.h"
#include "WhiskUI/draw/glesdraw.h"   // W3dScreenHeight (flip Y del scissor)
#include "w3dTexture.h"              // SavePNG: la captura de verificacion
#include "w3dFilesystem.h"
#include "w3dlog.h"

// ============================================================================
//  W3D_GAME_3D — el juego tiene ESCENA 3D.
//
//  Lo define "Compilar juego" cuando el proyecto tiene algo que dibujar en 3D
//  (una malla, una camara, una curva, un espejo...). Con el define, el binario
//  linkea las piezas COMPARTIDAS con el editor -- el lector de proyecto, los
//  objetos de escena, el pase 3D -- y este archivo agrega la carga de la escena
//  y el pase 3D antes del HUD.
//
//  Sin el define, un juego 2D puro (whiskpaddle, la calculadora) se compila
//  EXACTAMENTE como antes: nada de esto entra al binario.
// ============================================================================
#ifdef W3D_GAME_3D
#include "render/SceneRender.h"     // EL pase 3D, el MISMO que dibuja el viewport del editor
#include "render/RenderOptions.h"   // g_renderBg / g_showOverlays / flags de dibujo del Core
#include "importers/import_w3d.h"   // W3dProjectLoadScene3D: el MISMO lector que el editor
#include "objects/Camera.h"         // CameraActive + DeclaredAspect (el encuadre del juego)
#include "objects/Light.h"          // apagar los GL lights antes del pase
#include "objects/Mesh.h"
#include "animation/Animation.h"          // CurrentFrame / StartFrame / AnimIsGame / PlayAnimation
#include "animation/VertexAnimation.h"    // UpdateAnimations (el mundo 3D respira)
#include "animation/Armature2DAnimation.h"// Armature2DEvaluateScene (clips del rig 2D)
#include "objects/Materials.h"            // UpdateAnimatedMaterials / UpdateUVAnims
#include "W3dRoots.h"                    // changeScene() a otra ESCENA 3D del proyecto (descarga + carga)
#include "objects/Routine.h"               // W3dHasConstructor: el proyecto limpia la pantalla (sus pasos "Clear")
#endif

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include <vector>
#include <string>
#include <math.h>   // fabsf() para collide()

namespace gfx = w3dEngine;

// ---------------------------------------------------------------------------
//  GLOBALS que las piezas reusadas del editor esperan (definidas ACA para el
//  runtime; en el editor las definen variables.cpp / RenderOptions.cpp).
// ---------------------------------------------------------------------------
#ifndef W3D_GAME_3D
bool  g_redraw = true;                 // el editor lo usa para redibujar por evento
#endif                                 // (con 3D lo define libs/Whisk3DCore/render/RenderOptions.cpp)
float g_gameRenderW = 480.0f;          // tamano de la "ventana de render" del juego
float g_gameRenderH = 480.0f;          // (UIOverlay lo lee via UI2D_WindowSize)

// ZONA SEGURA: insets (notch/barras) en px de ventana, los pone la plataforma (Android). El fondo (clear)
// llena TODA la ventana (se ve detras del notch); la UI se acota a este rect. 0 = sin recorte (PC/web).
static float g_safeL = 0, g_safeT = 0, g_safeR = 0, g_safeB = 0;
void W3dGameSafeArea(int l, int t, int r, int b) {
    g_safeL = (float)(l < 0 ? 0 : l); g_safeT = (float)(t < 0 ? 0 : t);
    g_safeR = (float)(r < 0 ? 0 : r); g_safeB = (float)(b < 0 ? 0 : b);
}
// pedido de cierre del juego (lua quit()); la plataforma lo consulta cada frame y corta el loop.
// El flag vive en BindsJuego (donde esta el bind quit()), compartido con el editor.
bool W3dGameWantsStop() { return BindsGameWantsStop(); }

// la tipografia pixel (font.png); Fuente2D la pide con W3dGameFontPng()
static std::string gFontPng = "font.png";
void W3dGameFontPngSet(const std::string& path) { gFontPng = path; }
const char* W3dGameFontPng() { return gFontPng.c_str(); }

// ---------------------------------------------------------------------------
//  STUBS de imagen/video: el juego es solo rectangulos + texto, no usa texturas
//  ni video. UIOverlay igual referencia estas funciones (para los otros tipos),
//  asi que se stubean -> el runtime no arrastra ffmpeg ni el cache de video.
//  (Un juego con imagenes linkearia el Textura2D/Video2DCache reales en su lugar.)
// ---------------------------------------------------------------------------
#ifdef W3D_GAME_NO_TEX
// el juego SI usa una imagen (el parlante) -> Texture2DGet/RutaDecodificable REALES (Texture2D.cpp en
// el build). Pero NO usa video: se stubea solo el video para no arrastrar el cache de video / ffmpeg.
struct VideoPreview;
const VideoPreview* Video2DPreview(const std::string&) { return 0; }
unsigned W3dMsNow() { return 0; }
#endif

// ---------------------------------------------------------------------------
//  BINDINGS 2D del juego: AHORA viven en BindsGame.{h,cpp} (compartidos con el
//  editor), y se registran via W3dScriptSetBindExtra(BindsGameRegister) en
//  W3dGameLoadUI. Aca solo queda lo PROPIO del runtime: el UI del juego, el
//  mapeo ventana<->canvas del input, y el alimentar el estado de BindsJuego.
// ---------------------------------------------------------------------------
static UI* UIGame() {
    // con el juego corriendo manda la escena ACTIVA (su lienzo/tamano); antes de
    // W3dGameStart (gActive NULL) cae a la primera UI de la escena.
    UI* act = W3dActiveScene();
    if (act) return act;
    if (!SceneCollection) return 0;
    for (size_t i = 0; i < SceneCollection->Childrens.size(); i++)
        if (SceneCollection->Childrens[i]->getType() == ObjectType::ui)
            return (UI*)SceneCollection->Childrens[i];
    return 0;
}

// mapeo del ULTIMO render (ventana -> lienzo) para el INPUT (WinToCanvas): escala + offset del letterbox /
// zona segura. Lo fija W3dGameRender cada frame (y ademas se lo pasa a BindsJuego para safeArea/uiBox).
static float gLastEsc = 1.0f, gLastX0 = 0.0f, gLastY0 = 0.0f;

// ---------------------------------------------------------------------------
//  Estado del juego cargado
// ---------------------------------------------------------------------------
static Object* gOwner = 0;   // el objeto que lleva los scripts (la UI misma)

bool W3dGameLoadUI(const std::string& path) {
    // la RAIZ de la escena (el editor usa un Scene; el runtime, un Object pelado):
    // los objetos nuevos se cuelgan de aca, y UI2DLoad necesita que exista
    if (!SceneCollection) SceneCollection = new Object(0, "Scene");
    W3dScriptSetBindExtra(BindsGameRegister);   // registrar la API 2D compartida antes de cargar scripts
    UI* u = UI2DLoad(path);
    if (!u) return false;
    gOwner = u;
    g_gameRenderW = u->width; g_gameRenderH = u->height;
    return true;
}

// MULTI-ESCENA: carga UNA escena mas (un .w3dui) y la ACUMULA en SceneCollection (UI2DLoad
// se auto-cuelga sin pisar las anteriores). Sus scripts salen del propio .w3dui (array root
// "scripts"), no se agregan con W3dGameScript. Llamar una vez por escena, antes de W3dGameStart.
bool W3dGameLoadScene(const std::string& path) {
    if (!SceneCollection) SceneCollection = new Object(0, "Scene");
    W3dScriptSetBindExtra(BindsGameRegister);   // la API 2D compartida (incluye changeScene)
    UI* u = UI2DLoad(path);
    if (!u) return false;
    return true;
}
// nombre de la escena que ARRANCA (el campo "nombre" de su .w3dui). Sin esto arranca la primera.
void W3dGameInitialScene(const std::string& name) { W3dSceneSetInitial(name); }

// ---------------------------------------------------------------------------
//  ESCENA 3D: el project.json que dejo "Compilar juego" al lado del binario.
//  Lo lee el MISMO import_w3d que el editor (ver W3dProjectLoadScene3D):
//  aca solo se levantan los bytes -- por w3dFileSystem, asi sale igual de un
//  archivo suelto, del pak embebido o del APK -- y se marca que hay 3D.
// ---------------------------------------------------------------------------
static bool gHas3D = false;
bool W3dGameHas3D() { return gHas3D; }

#ifndef W3D_GAME_3D
// juego 2D puro: no hay lector de escena linkeado. Se avisa y el juego sigue.
bool W3dGameLoadScene3D(const std::string& path) {
    w3dLogfE("[juego] este juego se compilo SIN escena 3D; '%s' se ignora", path.c_str());
    return false;
}
#else
bool W3dGameLoadScene3D(const std::string& path) {
    if (!SceneCollection) SceneCollection = new Object(0, "Scene");
    // ------------------------------------------------------------------
    //  LOS BINDS COMPARTIDOS, TAMBIEN EN UN JUEGO 3D. Esto se registraba SOLO
    //  en W3dGameLoadUI / W3dGameLoadScene, o sea en los caminos 2D. Un
    //  juego 3D carga su escena por ACA y no pasa por ninguno de los dos: sus
    //  scripts arrancaban SIN el set de BindsJuego entero -- sound(),
    //  screen(), getPosPx(), pressed(), setRail(), getRail(), setSector(),
    //  setVisCurveT(), emit(), changeScene(), cameraXZ()... todos nil.
    //  El .lua del juego se caia en la primera llamada ("attempt to call a nil
    //  value") y con el se caia la mitad de abajo de su update(): sin
    //  sonidos, sin musica, sin la visibilidad por nodo y sin responder al
    //  mando. ESTE era el "no tiene nada que ver con como se ve en el editor".
    // ------------------------------------------------------------------
    W3dScriptSetBindExtra(BindsGameRegister);
    std::vector<unsigned char> data;
    if (!w3dFileSystem::ReadFileBytes(path, data) || data.empty()) {
        w3dLogfE("[juego] no pude leer la escena 3D '%s' (el juego abre sin ella)", path.c_str());
        return false;
    }
    if (!W3dProjectLoadScene3D(&data[0], data.size())) {
        w3dLogfE("[juego] la escena 3D '%s' no parsea (el juego abre sin ella)", path.c_str());
        return false;
    }
    gHas3D = true;
    // MODO JUEGO: es lo que el editor pone al dar Play (AnimIsGame + PlayAnimation),
    // y de lo que dependen las animaciones de la escena y el encuadre duro de la
    // camara. En un juego compilado siempre esta corriendo: no hay Stop.
    // ActiveAnimKind 2 ("Juego" en el selector del timeline, lo mismo que pone el editor al
    // abrir un proyecto con scripts): con el, CADA esqueleto reproduce su clip activo con su
    // propio cabezal, sus capas del mix y su transicion (EvaluatePoseSkeleton), y cada rig 2D
    // su clip (Armature2DAnimation). Sin esto quedaba en 0 (escena): W3dArmaturesGameTick
    // avanzaba los cabezales pero la pose no los leia y los personajes quedaban en bind.
    AnimIsGame = true;
    PlayAnimation = true;
    ActiveAnimKind = 2;
    return true;
}
#endif // W3D_GAME_3D

void W3dGameScript(const std::string& pathLua) {
    if (!gOwner) return;
    if (!gOwner->scriptData) gOwner->scriptData = new W3dScriptData();
    W3dScriptEntry e; e.path = pathLua;
    gOwner->scriptData->scripts.push_back(e);
}
void W3dGameRef(const std::string& prop, const std::string& value) {
    if (!gOwner || !gOwner->scriptData || gOwner->scriptData->scripts.empty()) return;
    gOwner->scriptData->scripts.back().refs.push_back(std::make_pair(prop, value));
}

static Object* FindByName(Object* o, const std::string& n) {
    if (!o) return 0;
    if (o->name == n) return o;
    for (size_t i = 0; i < o->Childrens.size(); i++) {
        Object* r = FindByName(o->Childrens[i], n);
        if (r) return r;
    }
    return 0;
}

// init de UN subarbol: carga+resuelve+start() de cada objeto con script (orden de arbol).
// Las refs se buscan dentro de la RAIZ de la escena del dueno (no en toda SceneCollection),
// para que no colisionen nombres repetidos entre escenas. reiniciar=true recrea los lua_State.
static void RuntimeInitSubtree(Object* o, bool restart) {
    if (!o) return;
    if (o->scriptData && !o->scriptData->scripts.empty()) {
        if (restart) W3dScriptUnloadFrom(o);
        if (W3dScriptLoad(o)) {
            for (size_t e = 0; e < o->scriptData->scripts.size(); e++) {
                const W3dScriptEntry& ent = o->scriptData->scripts[e];
                for (size_t r = 0; r < ent.refs.size(); r++) {
                    // (por scope: adentro de una instancia de prefab, primero lo de SU instancia)
                    Object* obj = W3dSceneFindRef(o, ent.refs[r].second);
                    if (obj) W3dScriptResolveRef(o, (int)e, ent.refs[r].first, obj);
                    W3dScriptResolveOption(o, (int)e, ent.refs[r].first, ent.refs[r].second);
                    // el MISMO par alimenta las propiedades de VALOR (property() en lua)
                    W3dScriptResolveValue(o, (int)e, ent.refs[r].first, ent.refs[r].second);
                }
            }
            W3dScriptStart(o);
        }
    }
    for (size_t i = 0; i < o->Childrens.size(); i++)
        RuntimeInitSubtree(o->Childrens[i], restart);
}
// callback de init de UNA escena (lo registra W3dSceneSetInit): lo llama el motor
// al arrancar la inicial y en el init perezoso de cada escena destino de changeScene().
static void RuntimeInitScene(UI* u, bool restart) { RuntimeInitSubtree((Object*)u, restart); }
// instantiate() de lua (un prefab del proyecto, io/Prefabs.h): lo nuevo arranca como lo del principio del juego
// (sus cuerpos rigidos se crean ANTES de sus start(), como en W3dGameStart: un physicsVel() en el start() de
// lo instanciado encuentra su cuerpo; sus scripts cargan, resuelven sus refs e inician). destroy() no necesita
// nada propio: el Core descarga sus scripts y lo libera al final del frame (W3dScriptDestroyObject).
// Su update() corre desde el frame SIGUIENTE, igual que en el Play del editor (gScriptedNew, SimGame.cpp):
// lo creado en este frame queda anotado (por serial) y el recorrido de los update() lo saltea hasta el final
// del frame. Sin esto, lo instanciado al final del arbol corria su update() en el mismo frame en que nacio
// y cada objeto creado jugando contaba un update() mas en el juego compilado que en el Play.
struct NewRuntime { Object* o; unsigned serial; };
static std::vector<NewRuntime> gFrameNew;
static void RuntimeNewObject(Object* root) {
    if (!root) return;
    NewRuntime n; n.o = root; n.serial = root->serial;
    gFrameNew.push_back(n);
    W3dRigidEnsure(root);
    RuntimeInitSubtree(root, false);
    W3dRigidEnsure(root);   // (lo que esos start() importaron: Asegurar es idempotente)
}
static bool RuntimeSpawnedThisFrame(const Object* o) {
    for (size_t i = 0; i < gFrameNew.size(); i++)
        if (gFrameNew[i].o == o && gFrameNew[i].serial == o->serial) return true;
    return false;
}

// LOS OBJETOS 3D de la escena (todo lo que cuelga de la raiz y NO es una UI): sus
// scripts se inician/corren aparte del sistema de escenas 2D, que solo conoce UIs.
static bool IsUI(Object* o) { return o && o->getType() == ObjectType::ui; }

#ifdef W3D_GAME_3D
// changeScene() a otra ESCENA o JUEGO 3D del proyecto (W3dRoots.h), aplicado al final del frame: la
// raiz actual se DESCARGA (sus scripts, cuerpos, hitbox y animaciones; despues se destruye su arbol, que
// suelta sus recursos) y la pedida se CARGA desde su entrada y arranca como al principio del juego:
// un JUEGO arranca su simulacion; una ESCENA ademas reproduce su animacion (cinematica) desde el inicio.
// getShared() sobrevive: es lo que pasa datos de un nivel al siguiente.
static void RuntimeUnloadScripts(Object* o) {
    if (!o) return;
    if (W3dSceneIsLayerRoot(o)) return;   // (una escena cargada encima sigue corriendo: loadScene)
    if (o->scriptData && !o->scriptData->scripts.empty()) W3dScriptUnloadFrom(o);
    for (size_t i = 0; i < o->Childrens.size(); i++) RuntimeUnloadScripts(o->Childrens[i]);
}
static void RuntimeChangeScene3D(int idx) {
    RuntimeUnloadScripts(SceneCollection);
    W3dPhysicsClear();
    W3dRigidClear();
    W3dHitboxClear();
    { extern void W3dAnimSceneReset(); W3dAnimSceneReset(); }
    W3dAnimObjectsReset();   // (los clips de objeto de la escena que se va)
    BindsGameResetPointers();
    W3dSceneClear();
    if (!W3dRootLoadInActive(idx)) w3dLogfE("[juego] cambiarEscena: la escena %d no se pudo cargar", idx);
    W3dGameStart();   // UI, scripts 3D y cuerpos de la escena nueva (registra otra vez este callback)
}

// CINEMATICA (playScene, W3dRoots.h): la ESCENA ya es la raiz activa (el juego quedo intacto fuera
// del arbol activo). Al entrar se inician sus scripts; al salir se descargan (la escena, si se cargo para
// esto, la libera W3dRoots al volver: el juego compilado no guarda lo que no usa).
static void RuntimeCinematic(int moment) {
    if (!SceneCollection) return;
    if (moment == W3D_CINEMATIC_ENTERED)
        for (size_t i = 0; i < SceneCollection->Childrens.size(); i++) RuntimeInitSubtree(SceneCollection->Childrens[i], false);
    else if (moment == W3D_CINEMATIC_EXITS)
        RuntimeUnloadScripts(SceneCollection);
}
// update() de los scripts de la escena de la cinematica (los visibles; UI incluidas: su "saltar")
static void RuntimeUpdateCinematic(Object* o, float dt) {
    if (!o) return;
    if (o->visible && o->scriptData && !o->scriptData->scripts.empty()) W3dScriptUpdate(o, dt);
    for (size_t i = 0; i < o->Childrens.size(); i++) RuntimeUpdateCinematic(o->Childrens[i], dt);
}
#endif

void W3dGameStart() {
    // RED DE SEGURIDAD: los scripts se CARGAN de aca para abajo, asi que el
    // registrador de binds tiene que estar puesto SI O SI antes. Los tres
    // caminos de carga (UI, multi-escena, 3D) ya lo ponen; esto cubre cualquier
    // camino nuevo que se olvide -- el sintoma de no tenerlo es carisimo de
    // diagnosticar (el juego abre, dibuja, y el .lua se cae por frame).
    W3dScriptSetBindExtra(BindsGameRegister);
    W3dScriptSetNewObject(RuntimeNewObject);   // instantiate(): los scripts y los cuerpos de lo nuevo
#ifdef W3D_GAME_3D
    if (gHas3D) W3dRootsSetChangeGame(RuntimeChangeScene3D);   // changeScene() a otra escena 3D
    if (gHas3D) W3dRootsSetCinematic(RuntimeCinematic);                      // playScene(): una cinematica
    // hasta el PRIMER cuadro dibujado no se sabe el aspecto de la imagen (RenderScene3D lo pone en cada cuadro). En 0,
    // los start() que usan screenRay/screenOf caen al aspecto del lienzo (ProjectToScreen/MatrixScreen) en vez del 4:3
    // por defecto. Solo la primera vez: en un changeScene() ya vale el de la ventana.
    { static bool first = true; if (first) { first = false; g_renderAspect = 0.0f; } }
#endif
    // CUERPOS RIGIDOS: crearlos ANTES de los start() (igual que SimPlay del editor). Sin esto
    // el juego compilado no simulaba NINGUN cuerpo: physicsVel/contacts() no hacian nada y todo
    // lo que en el Play caia, chocaba o se empujaba, en el binario quedaba quieto.
    W3dRigidClear();
    W3dRigidEnsure(SceneCollection);
    W3dHitboxClear();   // la partida arranca sin nadie "adentro" de ningun hitbox
    W3dSceneSetInit(RuntimeInitScene);
    W3dSceneRegisterAll();
    // inicial: la fijada con W3dGameInitialScene()/el .w3d; sino la escena del camino LEGACY
    // (gOwner, W3dGameLoadUI); sino la primera registrada (fallback de W3dSceneStart).
    std::string start = W3dInitialScene();
    if (start.empty() && gOwner) start = gOwner->name;
    W3dSceneSetInitial(start);
    if (W3dSceneHasScenes()) W3dSceneStart();   // muestra + inicia SOLO la escena inicial
    else if (gOwner)          RuntimeInitSubtree(gOwner, false);   // defensivo (no deberia pasar)
    // ESCENA 3D: los scripts colgados de objetos 3D (el gameplay de un juego 3D vive
    // ahi, no en el HUD). Van DESPUES de la UI para que sus refs encuentren todo.
    if (gHas3D && SceneCollection)
        for (size_t i = 0; i < SceneCollection->Childrens.size(); i++)
            if (!IsUI(SceneCollection->Childrens[i]) && !W3dSceneIsLayerRoot(SceneCollection->Childrens[i]))
                RuntimeInitSubtree(SceneCollection->Childrens[i], false);   // (las capas ya corren: loadScene)
    // SEGUNDA pasada de cuerpos rigidos: lo que los start() importaron con importW3D()
    // (Asegurar es idempotente: los ya creados no se tocan). Mismo criterio que SimPlay.
    W3dRigidEnsure(SceneCollection);
#ifdef W3D_GAME_3D
    // STREAMING: las instancias DIFERIDAS que estan cerca se generan YA (el primer frame esta completo) y arrancan como
    // lo instanciado, despues de los start() de la escena (igual que SimPlay del editor)
    if (gHas3D) { extern void W3dStreamingSessionStart(); W3dStreamingSessionStart(); }
    // la raiz es una ESCENA (cine, no un juego): reproduce su animacion desde el inicio, una vez
    // (sus scripts la pueden seguir con animSceneCurrent() y pedir la siguiente al terminar). Un
    // JUEGO no hace nada aca: ya arranco su simulacion (W3dRoots.h)
    if (gHas3D) W3dRootStartCinematic();
#endif
}

// corre update() de los objetos con script del subarbol de la escena ACTIVA (y visibles). Lo que instantiate()
// creo en ESTE frame no (su update(), desde el proximo: ver RuntimeNewObject)
static void RuntimeUpdateSubtree(Object* o, float dt) {
    if (!o) return;
    if (!gFrameNew.empty() && RuntimeSpawnedThisFrame(o)) return;
    if (W3dSceneIsFromActive(o) && o->scriptData && !o->scriptData->scripts.empty())
        W3dScriptUpdate(o, dt);
    for (size_t i = 0; i < o->Childrens.size(); i++)
        RuntimeUpdateSubtree(o->Childrens[i], dt);
}

// UN PASO DEL MUNDO 3D: lo mismo que hace el loop del editor una vez por frame con
// el juego corriendo (ver main/app/main.cpp, el bloque de UpdateAnimations). Sin
// esto la escena carga pero queda CONGELADA: nada de vertex anims, texturas
// animadas, tiras UV, particulas, visibilidad por celda ni clips de armature 2D.
#ifdef W3D_GAME_3D
extern void W3dCamerasRailTick();   // (libs/Whisk3DCore/objects/Camera.cpp) hornea el indice del riel
static void WorldStep3D(float dt) {
    UpdateAnimations(dt);
    UpdateAnimatedMaterials(dt);
    UpdateUVAnims(dt);
    { extern void W3dParticlesTick(float); W3dParticlesTick(dt); }
    { extern void W3dVisZonesTick();        W3dVisZonesTick(); }
    Armature2DEvaluateScene(CurrentFrame);
}
// el PLAYHEAD del juego (lo leen los clips y las curvas de animacion de escena):
// un tick = un frame, igual que el contador gTick del Play del editor.
static void WorldTick3D() { static int tick = 0; tick++; CurrentFrame = StartFrame + tick; }
#else
static void WorldStep3D(float) {}
static void WorldTick3D() {}
static void W3dCamerasRailTick() {}
#endif

void W3dGameUpdate(float dt) {
#ifdef W3D_GAME_3D
    // UNA CINEMATICA (playScene): el juego en PAUSA (su arbol ni esta en el activo: ni fisica ni
    // scripts ni animaciones suyas) y corre la ESCENA: su mundo, su animacion, sus clips, sus esqueletos y
    // sus scripts. Mismo orden que el tick de cinematica del Play del editor (TickCinematic en SimGame.cpp).
    if (gHas3D && W3dCinematicActive()) {
        WorldStep3D(dt);
        W3dCamerasRailTick();
        if (SceneCollection) RuntimeUpdateCinematic(SceneCollection, dt);
        W3dCinematicTick(dt);
        W3dAnimObjectsTick(dt);
        { extern void W3dArmaturesGameTick(float dt); W3dArmaturesGameTick(dt); }
        BindsGameSnapshotPointers();
        W3dScriptEndFrame();
        W3dSceneApplyPending();   // la vuelta al juego (termino / stopScene3D) y su onFinish()
        { extern void LoadPendingTextures(); LoadPendingTextures(); }
        { extern void W3dResourcesPump();          W3dResourcesPump(); }
        return;
    }
#endif
    // LA BASE EN PAUSA (pauseScene con su nombre: un menu de pausa cargado encima): el mundo no avanza (su
    // fisica, sus hitbox, sus animaciones; sus scripts los frena W3dSceneIsFromActive). Las capas siguen
    const bool world = !W3dSceneBasePaused();
    // EL MUNDO 3D primero (animaciones, particulas, visibilidad): mismo orden que el
    // editor, donde UpdateAnimations corre antes del tick de simulacion.
    if (gHas3D && world) WorldStep3D(dt);
    // FISICA del Core (velocidad + rebotes): PRIMERO se integran las posiciones y despues corren
    // los scripts, asi el script ve la posicion nueva y resuelve el choque (rebotar) en el MISMO
    // frame. Mismo orden y mismo dt que el Play del editor (TickReal en main/script/SimGame.cpp).
    if (world) W3dPhysicsStep(dt);
    // CUERPOS RIGIDOS + HITBOX: el mismo orden que TickReal del editor (fisica -> rigidos ->
    // hitbox -> scripts): los scripts ven los contacts() y los eventos alEntrar/alSalir/
    // alTocar de ESTE paso antes de su update().
    if (world) { W3dRigidStep(dt); W3dHitboxStep(dt); }
    // CAMARAS CON RIEL: hornear el indice fraccionario ANTES de los scripts, para que
    // getRail() lea el valor de ESTE tick (identico a TickReal del editor).
    if (gHas3D) W3dCamerasRailTick();
    UI* act = W3dActiveScene();
    if (act)             RuntimeUpdateSubtree((Object*)act, dt);
    else if (gOwner)    W3dScriptUpdate(gOwner, dt);   // legacy antes de Inicio (no deberia pasar)
    // ...y los scripts de los objetos 3D (el gameplay). W3dSceneIsFromActive deja pasar
    // todo lo que no cuelga de una UI, asi que la regla de visibilidad es la de siempre.
    if (gHas3D && SceneCollection)
        for (size_t i = 0; i < SceneCollection->Childrens.size(); i++)
            if (!IsUI(SceneCollection->Childrens[i]))
                RuntimeUpdateSubtree(SceneCollection->Childrens[i], dt);
    // LAS ESCENAS 2D CARGADAS ENCIMA (loadScene): corren aunque no sean la escena activa (las 3D ya corrieron
    // arriba: cuelgan de la raiz)
    for (int c = 0; c < W3dSceneLayers(); c++) {
        Object* r = W3dSceneLayerRoot(c);
        if (r && !W3dSceneLayerEs3D(c) && r != (Object*)act) RuntimeUpdateSubtree(r, dt);
    }
    // ANIMACION DE ESCENA pedida por los scripts (animScene de lua): avanzar el reloj y aplicar
    // las curvas, DESPUES de los scripts (el animScene() recien pedido pose en ESTE frame). Es el
    // mismo lugar que TickReal del editor; antes no se llamaba y animScene() no avanzaba nunca en
    // el juego compilado.
    if (world) { extern void W3dAnimSceneTick(float dt); W3dAnimSceneTick(dt); }
    // CLIPS DE OBJETO (animObject de lua): cada objeto con su cabezal, varios a la vez (mismo lugar que el
    // Play del editor)
    if (world) { extern void W3dAnimObjectsTick(float dt); W3dAnimObjectsTick(dt); }
    // SNAPSHOT del estado apretado para el flanco de pressed() del PROXIMO frame: se hace DESPUES de correr
    // los scripts, asi todas las llamadas a pressed() de ESTE frame ven el mismo "estado anterior".
    // ESQUELETOS: cada armature avanza su clip con su propio reloj (animClip de lua), mismo lugar que el Play
    // del editor (TickReal en main/script/SimGame.cpp)
    if (world) { extern void W3dArmaturesGameTick(float dt); W3dArmaturesGameTick(dt); }
    BindsGameSnapshotPointers();
#ifdef W3D_GAME_3D
    // STREAMING (main/io/Streaming.h): las instancias DIFERIDAS cargan lo que se acerco y descargan lo que se alejo,
    // fuera de los update() y antes del fin de frame (lo que genera corre su update() desde el proximo). Mismo
    // lugar que el Play del editor (TickReal en main/script/SimGame.cpp)
    if (gHas3D) { extern void W3dStreamingTick(); W3dStreamingTick(); }
#endif
    W3dScriptEndFrame();   // flanco de keyPressed(): UNA vez por frame, despues de TODOS los objetos con script
    // lo que instantiate() creo en este frame corre su update() desde el proximo (mismo lugar que el Play del
    // editor, que suma sus nuevos a la lista despues de W3dScriptEndFrame)
    gFrameNew.clear();
    // el cambio pedido por changeScene() se aplica ACA, al final del frame (no en el callback:
    // no mutar la iteracion de arriba). El init perezoso de la escena destino corre en este punto.
    W3dSceneApplyPending();
    if (gHas3D) WorldTick3D();   // el playhead del juego avanza un frame por tick
    // ---- LO QUE EL EDITOR HACE UNA VEZ POR FRAME Y ACA FALTABA ---------------
    // El main loop del editor (main/app/main.cpp) corre estas dos DESPUES del tick
    // de simulacion, y son las que hacen aparecer lo que se carga TARDE:
    //   * LoadPendingTextures: la cola diferida del importador (1 textura por
    //     frame). El juego arranca con LoadAllPendingTextures, pero todo lo
    //     que se importe/instancie DESPUES (un prefab que aparece a mitad de nivel)
    //     encolaba su textura y no la decodificaba NADIE: se veia gris para siempre.
    //   * W3dResourcesPump: la bomba del almacen de recursos (streaming ASYNC de las
    //     listas de carga). Sin ella un recurso pedido ASYNC queda EN_VUELO para
    //     siempre y Resolve() devuelve NULL: el nodo que lo espera no aparece nunca
    //     (agujeros en el mapa). Cola vacia = no-op, asi que no cuesta nada.
    // (QUEDA PENDIENTE la tercera, ApplyAnimationObjects: vive en ObjectMode.cpp,
    //  que es codigo de EDICION y no se linkea en el juego. Afecta solo a las curvas
    //  de animacion de ESCENA por keyframes -- no a las vertex anims ni a los scripts.)
#ifdef W3D_GAME_3D
    // (la cola diferida vive en import_obj.cpp, que solo linkea el juego 3D)
    { extern void LoadPendingTextures(); LoadPendingTextures(); }
#endif
    { extern void W3dResourcesPump();          W3dResourcesPump(); }
}

// ---------------------------------------------------------------------------
//  EL PASE 3D del juego. Dibuja la escena dentro del rect seguro con el ENCUADRE
//  declarado por la camara (`aspecto:` propio o heredado del riel) y tapa lo de
//  afuera con BARRAS NEGRAS OPACAS -- que es la diferencia con el editor, donde
//  las barras van atenuadas para poder auditar el culling.
//
//  Todo lo que dibuja sale de libs/Whisk3DCore/render/SceneRender.cpp, el MISMO archivo que
//  usa Viewport3D::Render: proyeccion, camara, recorrido de la escena (culling,
//  LOD, visibilidad por celdas, espejos, instancias, luces, lotes), calcomanias,
//  luces aditivas y particulas, en ese orden.
//
//  Devuelve en 'marco' el rect (px de ventana) donde quedo la imagen: ahi adentro
//  va el HUD, para que quede alineado con lo que se ve (igual que en el editor).
// ---------------------------------------------------------------------------
static void CaptureTick(int winW, int winH);   // (definida al final: la captura de verificacion)

// (el medidor del modo debug del main generado: cuanto tardaron el pase 3D y el HUD en el ultimo cuadro)
static float gMs3D = 0.0f, gMs2D = 0.0f;
void W3dGameTimes(float* ms3d, float* ms2d) {
    if (ms3d) *ms3d = gMs3D;
    if (ms2d) *ms2d = gMs2D;
}

static void RenderScene3D(int winW, int winH, float sl, float st, float safeW, float safeH,
                           float* mx0, float* my0, float* mw, float* mh) {
    *mx0 = sl; *my0 = st; *mw = safeW; *mh = safeH;
#ifndef W3D_GAME_3D
    (void)winW; (void)winH;   // juego 2D puro: no hay pase 3D compilado
#else
    if (!gHas3D || !SceneCollection) return;

    // el juego NO dibuja chrome del editor: ni contornos de seleccion, ni gizmos,
    // ni iconos de luces/camaras/empties. Se apaga una vez por frame, antes del pase.
    g_showOverlays    = false;
    w3dRenderOverlays = false;
    g_showLights = false; g_showCamera = false; g_showEmpty = false;

    // LUCES del pase: se apagan las 8 de GL; cada luz de la escena prende la suya al dibujarse. Igual
    // que ReloadLights en el editor, va al PRINCIPIO del frame. EL JUEGO DIBUJA SIEMPRE EN MODO RENDER: los
    // modos de vista (Material, Solid, Wireframe...) son del viewport del editor, no del juego
    W3dScene3DLights((int)RenderType::Rendered);

    const int glY = winH - (int)(st + safeH);   // arbol arriba-izq -> GL (origen abajo)
    gfx::Viewport((int)sl, glY, (int)safeW, (int)safeH);
    gfx::Disable(gfx::Fog);
    // un proyecto con CONSTRUCTOR limpia el solo (sus pasos "Clear color" y "Clear"; sin "Clear" no se limpia nada);
    // uno viejo, con el fondo del render
    if (!W3dHasConstructor() || !W3dScene3DActive()) {
        gfx::ClearColor(g_renderBg[0], g_renderBg[1], g_renderBg[2], g_renderBg[3]);
        gfx::Clear(gfx::ColorBuffer | gfx::DepthBuffer);
    }
    gfx::Disable(gfx::Texture2D); gfx::Disable(gfx::Blend); gfx::Disable(gfx::ColorMaterial);
    gfx::EnableArray(gfx::VertexArray);
    gfx::DisableArray(gfx::TexCoordArray); gfx::DisableArray(gfx::NormalArray);
    gfx::Enable(gfx::DepthTest);
    // RENORMALIZAR LAS NORMALES tras escalar (lo mismo que prende el editor al arrancar, W3dInitUI.cpp). Sin esto, en el
    // pipeline fijo una malla ESCALADA se iluminaba mal: las normales salian estiradas por la escala (mas claro con escala
    // < 1, mas oscuro con > 1) y con un eje muy fino (una tapa chata, una varilla) las caras que miran por ese eje
    // quedaban NEGRAS o quemadas. En el editor se veia bien y en el juego compilado no
    gfx::Enable(gfx::Normalize);

    // la camara del juego + su ENCUADRE
    W3dView3D v;
    float aspView = (safeH > 0.5f) ? (safeW / safeH) : 1.0f;
    v.viewAspect  = aspView;
    if (CameraActive) {
        CameraActive->UpdatePosition();     // riel + mirada (lo mismo que hace el viewport)
        CameraActive->UpdateLookAt();
        v.cam.pos = CameraActive->pos;
        v.cam.rot = CameraActive->Rot();
        v.fov   = CameraActive->fov;
        v.nearC = CameraActive->nearClip;
        v.farC  = CameraActive->farClip;
        v.ortho = CameraActive->orthographic;
        float decl = CameraActive->DeclaredAspect();
        v.imageAspect = (decl > 0.01f) ? decl : aspView;
    } else {
        // sin camara en la escena: la vista por defecto (el juego igual se ve)
        v.cam.pos = Vector3(0, 1, 6);
        v.imageAspect = aspView;
    }
    // EL ASPECTO DEL RENDER = el de la imagen que se dibuja de verdad. En el editor lo fija el panel de Propiedades
    // (ancho/alto del render); en el juego compilado no hay panel y g_renderAspect se quedaba en el 4:3 por defecto.
    // Con una ventana de otro aspecto (16:9, el celular parado) screenRay/screenPoint/screenOf (MatrixScreen) y el
    // culling de la camara (Culling.cpp) calculaban con otro encuadre que el dibujado: el rayo no pasaba por donde
    // se tocaba y lo de los costados se podia recortar. Se actualiza cada cuadro: la ventana cambia y el celular gira.
    g_renderAspect = v.imageAspect;
    // margen 1.0 = el encuadre ocupa TODO lo que puede (sin aire): lo que ve el jugador
    W3dFramingFrame(aspView, v.imageAspect, 1.0f, &v.frameNX, &v.frameNY);
    // ORTOGRAFICA: el volumen visible no depende de la distancia, asi que hace falta
    // una MEDIDA. El viewport del editor usa su distancia de orbita (concepto suyo);
    // un juego no tiene orbita, asi que se usa la distancia de la camara a su TARGET
    // (lo que esta mirando) con el mismo tan(fov/2): al mirar por la camara el objeto
    // sale del mismo tamaño que en perspectiva. Sin target, 10 unidades.
    v.orthoSize = 1.0f;
    if (v.ortho) {
        float d = 10.0f;
        if (CameraActive && CameraActive->target) {
            Vector3 t = CameraActive->target->GetGlobalPosition();
            Vector3 p = v.cam.pos;
            float dx = t.x - p.x, dy = t.y - p.y, dz = t.z - p.z;
            float m = sqrtf(dx*dx + dy*dy + dz*dz);
            if (m > 0.01f) d = m;
        }
        v.orthoSize = d * tanf(v.fov * 0.5f * 3.14159265f / 180.0f);
        if (v.orthoSize < 0.001f) v.orthoSize = 0.001f;
    }

    W3dScene3DProjection(v);
    // MODO DE DIBUJO: el render, con las luces de la escena (la misma derivacion que el viewport del editor en
    // "Rendered", EscenaRender). No el modo de vista del editor (antes 'view': MaterialPreview por defecto, y el juego
    // salia con la luz de preview en vez de las suyas). Va DESPUES de la proyeccion (que deja la modelview en
    // identidad) y ANTES de la camara: la posicion de una luz de GL se transforma por la modelview del momento.
    W3dScene3DMode((int)RenderType::Rendered);
    W3dScene3DCamera(v, v.cam.pos, Vector3(1, 1, 1));
    W3dScene3DPass();

    // BARRAS del encuadre: en el juego SIEMPRE negro opaco (el aparato no muestra
    // lo de afuera del encuadre). Se dibujan en el rect seguro, en 2D.
    if (v.frameNX < 0.999f || v.frameNY < 0.999f) {
        gfx::Viewport((int)sl, glY, (int)safeW, (int)safeH);
        W3dScene3DBands(safeW, safeH, v.frameNX, v.frameNY, 1.0f, 0.0f, 0.0f, true);
    }
    // el rect de la IMAGEN, en px de ventana: ahi va el HUD
    *mw  = safeW * v.frameNX; *mh = safeH * v.frameNY;
    *mx0 = sl + (safeW - *mw) * 0.5f;
    *my0 = st + (safeH - *mh) * 0.5f;
#endif // W3D_GAME_3D
}

// dibuja la escena 3D (si hay) y encima la UI 2D del juego (el HUD)
void W3dGameRender(int winW, int winH) {
    if (winW < 1) winW = 1; if (winH < 1) winH = 1;
    gfx::StatsFrame();   // (el contador de llamadas GL: cierra el cuadro anterior; la escena y el HUD declaran la suya)
    // el lienzo del juego: la ventana REAL si la UI es "igual que el render" (dinamico), o su tamaño
    // FIJO si no. Lo leen TANTO el script (CanvasSize) como el overlay (UI2D_WindowSize) -> coinciden.
    // zona SEGURA (descuenta notch/barras): la UI se dibuja adentro; el fondo (clear) llena TODA la ventana,
    // asi el color de fondo se ve DETRAS del notch pero la UI no queda tapada.
    float sl = g_safeL, st = g_safeT, sr = g_safeR, sb = g_safeB;
    if (sl + sr >= winW) { sl = 0; sr = 0; }
    if (st + sb >= winH) { st = 0; sb = 0; }
    float safeW = (float)winW - sl - sr, safeH = (float)winH - st - sb;
    W3dScreenHeight = winH;
    gfx::Viewport(0, 0, winW, winH);
    // (con escena 3D de un proyecto que limpia el solo, no: su "Clear" limpia la ventana entera -el notch tambien- con
    //  su color, y sin "Clear" no se limpia nada)
    bool clear = true;
#ifdef W3D_GAME_3D
    if (gHas3D && SceneCollection && W3dHasConstructor() && W3dScene3DActive()) clear = false;
#endif
    if (clear) {
        gfx::ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        gfx::Clear(gfx::ColorBuffer | gfx::DepthBuffer);
    }

    // ---- PASE 3D (si el juego tiene escena): la imagen del juego ----
    // El HUD se dibuja DENTRO del rect que devuelve (el encuadre), no sobre la
    // ventana entera: asi queda alineado con la imagen, igual que en el editor.
    float hx0, hy0, hW, hH;
    extern double W3dNowMs();
    const double t0 = W3dNowMs();
    RenderScene3D(winW, winH, sl, st, safeW, safeH, &hx0, &hy0, &hW, &hH);
    const double t1 = W3dNowMs();
    gMs3D = (float)(t1 - t0);

    // ---- PASE 2D (el HUD) ----
    UI* ug = UIGame();
    bool equal = (ug && ug->sameAsRender);
    if (equal) { g_gameRenderW = hW; g_gameRenderH = hH; }
    else if (ug) { g_gameRenderW = ug->width; g_gameRenderH = ug->height; }
    gfx::Viewport(0, 0, winW, winH);

    gfx::MatrixMode(gfx::Projection); gfx::LoadIdentity();
    gfx::Ortho(0, winW, winH, 0, -1, 1);
    gfx::MatrixMode(gfx::ModelView); gfx::LoadIdentity();
    // estado 2D (el MISMO que el Editor2D antes del overlay): sin depth/luz/getFog/
    // cull, y con el vertex array activo (los rects y el texto lo esperan asi)
    gfx::Disable(gfx::DepthTest); gfx::Disable(gfx::Lighting);
    gfx::Disable(gfx::Fog); gfx::Disable(gfx::CullFace);
    gfx::Disable(gfx::Blend); gfx::Disable(gfx::Texture2D);
    gfx::DisableArray(gfx::NormalArray); gfx::DisableArray(gfx::ColorArray);
    gfx::DisableArray(gfx::TexCoordArray); gfx::EnableArray(gfx::VertexArray);

    float lw, lh; UI2D_CanvasSize(&lw, &lh);
    if (lw < 1) lw = 1; if (lh < 1) lh = 1;
    float esc, dw, dh, x0, y0;
    if (equal) {
        // lienzo = la PANTALLA DEL JUEGO, 1:1 (sin letterbox propio): el rect seguro,
        // o el ENCUADRE de la camara cuando hay escena 3D con aspecto declarado
        esc = 1.0f; dw = hW; dh = hH; x0 = hx0; y0 = hy0;
    } else {
        // UI de tamaño fijo: encajar (letterbox) DENTRO de la pantalla del juego
        esc = hW / lw; if (hH / lh < esc) esc = hH / lh;
        dw = lw * esc; dh = lh * esc;
        x0 = hx0 + (hW - dw) * 0.5f; y0 = hy0 + (hH - dh) * 0.5f;
    }
    gLastEsc = esc; gLastX0 = x0; gLastY0 = y0;   // para convertir el toque a lienzo (input, WinToCanvas)
    // screenMargin() y screenFrame() de lua: la misma cuenta que el viewport del editor (ViewPort3D, la rama del
    // lienzo encajado). Solo la hacia el editor: en el juego compilado daban 0 y un HUD que se acomoda a la pantalla (los
    // medidores pegados arriba en vertical, los botones tactiles a los bordes) no se movia. Los bordes son los de la zona
    // segura (lo que se pega al borde no queda bajo un notch)
    { extern float g_hudMarginX, g_hudMarginY, g_hudFrame[4];
      if (equal) {
          g_hudMarginX = g_hudMarginY = 0.0f;
          g_hudFrame[0] = g_hudFrame[1] = 0.0f; g_hudFrame[2] = hW; g_hudFrame[3] = hH;
      } else if (esc > 0.0001f) {
          g_hudMarginX = (x0 - sl) / esc; g_hudMarginY = (y0 - st) / esc;
          if ((sl + safeW) - (x0 + dw) < x0 - sl) g_hudMarginX = ((sl + safeW) - (x0 + dw)) / esc;
          if ((st + safeH) - (y0 + dh) < y0 - st) g_hudMarginY = ((st + safeH) - (y0 + dh)) / esc;
          if (g_hudMarginX < 0.0f) g_hudMarginX = 0.0f;
          if (g_hudMarginY < 0.0f) g_hudMarginY = 0.0f;
          g_hudFrame[0] = (hx0 - (x0 + dw * 0.5f)) / esc; g_hudFrame[1] = (hy0 - (y0 + dh * 0.5f)) / esc;
          g_hudFrame[2] = hW / esc; g_hudFrame[3] = hH / esc;
      } }

    UI2D_BaseClip(0, 0, winW, winH);
    // pedirle al overlay las posiciones RESUELTAS (para que uiBox() las lea desde el lua)
    static std::vector<UI2DPos> positions;   // (reusado: sin volver a pedir memoria en cada cuadro)
    positions.clear();
    UI2D_DrawOverlay(x0, y0, dw, dh, esc, &positions);
    // alimentar a BindsJuego el mapeo + el rect SEGURO (px de ventana) + las posiciones resueltas,
    // para que safeArea()/uiBox() lean lo mismo que se dibujo (IGUAL que en el editor via SimJuego)
    BindsGameRender(esc, x0, y0, sl, st, safeW, safeH, &positions);
#ifdef W3D_GAME_3D
    // las capturas que pidio lua CON el HUD (captureScreen(nombre, true): los cortes con fundido), como el
    // viewport del editor despues de su RenderUI
    W3dCapturesDoWithHud();
#endif
    gMs2D = (float)(W3dNowMs() - t1);

    // ---- CAPTURA de verificacion (W3D_CAPTURA): el frame pedido a PNG y salir ----
    CaptureTick(winW, winH);
}

// ---------------------------------------------------------------------------
//  CAPTURA: guardar un frame a PNG y pedir el cierre. Es como se VERIFICA que un
//  juego compilado dibuja de verdad su escena (la prueba de regresion compara
//  pixeles del PNG). Sin W3D_CAPTURA no cuesta nada: un if por frame.
// ---------------------------------------------------------------------------
static std::string gCapPng;
static int gCapFrame = 0, gCapN = 0;
void W3dGameCapture(const std::string& png, int frame) {
    gCapPng = png; gCapFrame = (frame > 0) ? frame : 1; gCapN = 0;
}
static void CaptureTick(int winW, int winH) {
    if (gCapPng.empty()) return;
    if (++gCapN < gCapFrame) return;
    std::vector<unsigned char> px((size_t)winW * (size_t)winH * 4);
    gfx::ReadPixelsRGBA(0, 0, winW, winH, &px[0]);
    if (w3dEngine::SavePNG(gCapPng.c_str(), &px[0], winW, winH, true))
        w3dLogf("[juego] captura escrita: %s (%dx%d, frame %d)", gCapPng.c_str(), winW, winH, gCapN);
    else
        w3dLogfE("[juego] no pude escribir la captura %s", gCapPng.c_str());
    gCapPng.clear();
    BindsGameRequestExit();   // el loop de la plataforma corta al ver W3dGameWantsStop()
}

// ---- input ----
// convierte px de VENTANA a px del LIENZO (centro en 0,0) con el escalado del ultimo render. Lo comparten
// las 3 entradas de puntero (tap/finger/mouse): solo cambian a que funcion del script le pasan cx,cy.
static void WinToCanvas(int winX, int winY, float* cx, float* cy) {
    float lw, lh; UI2D_CanvasSize(&lw, &lh);
    if (lw < 1.0f) lw = 1.0f; if (lh < 1.0f) lh = 1.0f;
    *cx = 0; *cy = 0;
    if (gLastEsc > 0.0001f) {
        *cx = (winX - gLastX0) / gLastEsc - lw * 0.5f;
        *cy = (winY - gLastY0) / gLastEsc - lh * 0.5f;
    }
}
void W3dGameTouch(int winX, int winY, bool active) {
    float cx, cy; WinToCanvas(winX, winY, &cx, &cy); W3dScriptTouch(cx, cy, active);
    // cache en LIENZO para pressed()/etc (el toque = finger/slot 0)
    BindsGamePoint(0, cx, cy, active);
}
// MULTI-TOUCH: un dedo idx (0-based) -> finger(idx+1) en lua (2 jugadores tactiles, cada uno con su dedo)
void W3dGameFinger(int idx, int winX, int winY, bool active) {
    float cx, cy; WinToCanvas(winX, winY, &cx, &cy); W3dScriptFinger(idx, cx, cy, active);
    BindsGamePoint(idx, cx, cy, active);
}
// MOUSE-OVER: posicion del mouse SIN clic -> mouse() en lua (resaltar la opcion bajo el cursor)
void W3dGameMouse(int winX, int winY, bool inside) {
    float cx, cy; WinToCanvas(winX, winY, &cx, &cy); W3dScriptMouse(cx, cy, inside);
    // cache en LIENZO para el overlay (borde de hover de los botones). Solo los eventos
    // de MOUSE llegan aca: en tactil no se alimenta y no hay hover.
    BindsGameMouse(cx, cy, inside);
}
void W3dGameStick(int which, float x, float y) { W3dScriptStick(which, x, y); }
void W3dGameTrigger(int which, float v) { W3dScriptTrigger(which, v); }
void W3dGameButton(const char* name, bool v)  { W3dScriptButtonPad(name, v); }

#include <SDL2/SDL.h>
void W3dGameKey(int sdlk, bool pressed) {
    const char* n = 0; char letter[2] = { 0, 0 };
    if (sdlk >= SDLK_a && sdlk <= SDLK_z) { letter[0] = (char)('a' + (sdlk - SDLK_a)); n = letter; }
    else if (sdlk == SDLK_UP)    n = "up";
    else if (sdlk == SDLK_DOWN)  n = "down";
    else if (sdlk == SDLK_LEFT)  n = "left";
    else if (sdlk == SDLK_RIGHT) n = "right";
    else if (sdlk == SDLK_SPACE) n = "space";
    else if (sdlk == SDLK_LSHIFT || sdlk == SDLK_RSHIFT) n = "shift";   // igual que SimKeySDL del editor
    else if (sdlk == SDLK_LCTRL  || sdlk == SDLK_RCTRL)  n = "ctrl";
    else if (sdlk == SDLK_RETURN || sdlk == SDLK_KP_ENTER) n = "enter";
    else if (sdlk >= SDLK_0 && sdlk <= SDLK_9) { letter[0] = (char)('0' + (sdlk - SDLK_0)); n = letter; }
    if (n) W3dScriptKey(n, pressed);
}

// NOTIFICACIONES: en el editor son el cartelito de la esquina. En un juego no hay
// donde mostrarlas, asi que van al log (que en un build de debug se ve por consola).
// Va FUERA del bloque 3D: tambien la nombra el codigo compartido de un juego 2D puro
// (BindsJuego avisa con W3dAvisof cuando se conecta o desconecta un control).
void Notify(const std::string& msg, bool error) {
    if (error) w3dLogfE("[juego] %s", msg.c_str());
    else       w3dLogf("[juego] %s", msg.c_str());
}

#ifdef W3D_GAME_3D
// ============================================================================
//  LO QUE EL EDITOR PONE Y EL JUEGO NO TIENE.
//
//  El juego linkea las piezas COMPARTIDAS (el lector de proyecto, la edicion de
//  malla que genera la geometria, el pase 3D, los objetos de escena), y esas
//  piezas nombran un puñado de cosas que solo existen con editor delante: la
//  ventana de notificaciones, el proyecto abierto, el estado de la interfaz.
//  Aca estan, definidas UNA vez y con el comportamiento correcto para un juego.
//  No son "stubs para que linkee": son la version de juego de cada una.
// ============================================================================
#include "objects/Objects.h"

// EL PROYECTO ABIERTO: en el juego no hay "abrir otro .w3d"; la escena viene
// horneada al lado del binario. Queda vacio y las rutas resuelven por entrada.
std::string w3dPath;

// ESTADO DE LA INTERFAZ del editor que el traversal del Core consulta para dibujar
// (o no) los overlays. En el juego nunca hay overlays: false y listo.
bool showOverlayGlobal = false;
bool ViewFromCameraActiveGlobal = true;   // el juego SIEMPRE mira por la camara de la escena
bool g_showCamera = false;
bool g_showEmpty  = false;
// (g_showLights vive en el Core con los demas flags de dibujo)

// LOOP CUT: preview de corte del Edit Mode. En el juego no hay edicion.
bool gLoopCutIsPoint = false;
// CURSOR 3D y luces dibujadas: overlays del editor.
Cursor3D cursor3D;
bool g_showLights = false;
// GIZMOS de curvas (la linea del riel) y de emisores (la flecha): overlays del editor.
// Las particulas en si se dibujan igual; solo el gizmo queda apagado.
bool g_showCurves    = false;
bool g_showParticles = false;

// EL MIPMAPPING POR DEFECTO: el lector de proyecto lo pide cuando el .w3d no lo declara. En el editor es
// una opcion de Ajustes; el juego usa el de fabrica (prendido), lo mismo que ve el editor recien instalado.
bool W3dHostMipmapsDefault() { return true; }   // (el editor: cfg.mipmaps de Ajustes)

// EL PLAY DEL EDITOR (SimGame.cpp): en el juego compilado la simulacion corre
// SIEMPRE desde que arranca, y nunca rebobina (sin cache de snapshots).
bool gSimCacheOn = false;
bool SimActive() { return true; }
bool W3dGameRunning() { return true; }

// RANGO DE RENDER (tarjeta Render del editor: inicio/fin/fps para exportar video):
// el lector lo levanta del .w3d; el juego no exporta video, queda en 0 (= todo).
float g_renderStart = 0.0f, g_renderEnd = 0.0f, g_renderFps = 0.0f;

// ---- UNDO: un juego no tiene deshacer. Las capturas son no-op y los "destinos
//      de renombre" que las alimentan devuelven un destino vacio. ----
#include "base/W3dHost.h"   // (las declaraciones de todo esto: lo que el Core le pide al anfitrion)
void UndoCaptureMeshGeo(Mesh*) {}
void UndoCaptureRenames(const std::vector<W3dRenameDest>&) {}
void UndoMergeLast(int) {}
void UndoModRemove(Mesh*) {}
void UndoModEmpty(Mesh*) {}
void UndoModAdd(Mesh*, int) {}
void UndoModMove(Mesh*, int) {}
bool UndoMoveLayerMesh(Mesh*, int, int, int) { return false; }
W3dRenameDest W3dDestLayerMesh(Mesh*, int, int)             { return W3dRenameDest(); }
W3dRenameDest W3dDestBone3D(Armature*, int)                { return W3dRenameDest(); }
W3dRenameDest W3dDestBone2D(Mesh*, Armature2D*, int)       { return W3dRenameDest(); }
std::string*  W3dDestResolve(const W3dRenameDest&)         { return NULL; }

// ---- BARRA DE PROGRESO: es un popup del editor. En el juego no se dibuja. ----
void ProgressStart(const std::string&) {}
void ProgressUpdate(float) {}
void ProgressEnd() {}

// ---- IMPORTADORES DE MODELOS EXTERNOS (GLB/glTF/FBX): son del editor. Un
//      proyecto GUARDADO no los necesita: al guardar, toda la geometria queda en
//      el formato propio (.w3dm) y el .w3d referencia ESO. Si un proyecto viejo
//      todavia trae un nodo "glb", el juego avisa y sigue sin ese objeto en vez de
//      arrastrar el importador entero (y sus dependencias) al binario del juego.
bool ImportModelByExtension(const std::string& path) {
    w3dLogfE("[juego] '%s' es un modelo externo (GLB/glTF/FBX): el juego compilado solo "
             "lee la geometria propia (.w3dm). Volve a guardar el proyecto en el editor "
             "para convertirlo.", path.c_str());
    return false;
}
#else
// ---------------------------------------------------------------------------
//  JUEGO 2D PURO: el Core linkea Mesh (Objects/W3dScript lo referencian) pero el
//  juego NUNCA crea uno -> estos metodos de edit-mode viven en el editor y aca se
//  stubean para satisfacer al linker. Un juego 3D linkea los reales (MeshEdit.cpp).
// ---------------------------------------------------------------------------
#include "objects/Mesh.h"
// PointFocus es la KEY FUNCTION de Mesh (1er virtual): definirla aca emite la vtable
Vector3 Mesh::PointFocus() const { return Vector3(0,0,0); }
float   Mesh::FocusRadius() const { return 0.0f; }
void Mesh::EditSelectAll(bool) {}
void Mesh::EditInvert() {}
void Mesh::FreeModifiers() {}
void Mesh::FreeMeshModified() {}
void Mesh::FreeLayers(bool) {}
void Mesh::InvalidateEdit() {}
void Mesh::RenderEditOverlay() {}

// importW3D() (BindsJuego, compartido) ANEXA un .w3d con el LECTOR 3D, que un juego 2D
// puro no trae (import_w3d.cpp va en kSources3D). La version de juego 2D: avisa y devuelve
// NULL -> en lua importW3D() da nil, como con un archivo que no existe. Sin esto un juego
// sin nada 3D ni linkeaba.
Object* W3dImportW3DAppendix(const std::string& path) {
    w3dLogfE("[juego] importarW3D('%s'): este juego es 2D (no trae el lector de escenas 3D)",
             path.c_str());
    return NULL;
}

// las CAPTURAS de pantalla (captureScreen de lua, las texturas "pantalla:<nombre>", el paso "Copy screen" de una
// rutina) son del pase 3D (render/SceneRender.cpp): un juego 2D no tiene que capturar. Pedir una no hace nada y una
// imagen con "pantalla:..." queda sin textura
class Texture;
Texture* W3dCaptureTexture(const std::string&, bool) { return NULL; }
bool W3dIsCapturePath(const std::string& path) { return path.compare(0, 7, "screen:") == 0; }
void W3dCaptureRequest(const std::string&, bool) {}
bool W3dCaptureCopyNow(const std::string&) { return false; }
bool W3dCaptureUV(const std::string&, float uv[4]) { (void)uv; return false; }
// la CAMARA activa: la mira el paso de rutina "Skip if camera riel" (Routine.cpp). Un juego 2D no tiene camaras
class Camera;
Camera* CameraActive = NULL;
#endif // W3D_GAME_3D
