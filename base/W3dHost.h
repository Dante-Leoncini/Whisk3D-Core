#ifndef W3D_HOST_H
#define W3D_HOST_H
// ============================================================================
//  W3dHost.h - LO QUE EL PROGRAMA ANFITRION LE PONE AL CORE.
//
//  El Core (la escena, el lector de proyecto, el render, los scripts, la edicion de mallas) se usa desde dos
//  programas: el EDITOR y el JUEGO COMPILADO (game/w3drun.cpp). Hay un punado de cosas que el Core nombra y
//  que cada programa resuelve a su manera: los avisos al usuario, la barra de progreso de una carga larga,
//  el deshacer de las operaciones de malla, si el Play esta corriendo...
//
//  Se DECLARAN aca (el Core no incluye NADA del editor) y las DEFINE el anfitrion:
//    - el EDITOR, con las suyas (el cartelito de la esquina, el popup de progreso, el Ctrl+Z, SimGame);
//    - el JUEGO COMPILADO, con las de un juego (el log, nada, nada, "siempre corriendo"): game/w3drun.cpp.
//  Un programa propio que use el Core define estas mismas; las de w3drun.cpp son el punto de partida.
//
//  (Las banderas de los OVERLAYS del visor -g_showLights, cursor3D...- estan en render/RenderOptions.h,
//   con el resto de las opciones de dibujo; tambien las define el anfitrion.)
// ============================================================================
#include <string>
#include <vector>
#include "edit/W3dRenameDest.h"   // el DESTINO de un rename (W3dDest*, W3dDestResolve: tambien del anfitrion)

class Mesh;

// ---- la RUTA del proyecto abierto (el .w3d). En un juego queda vacia: la escena viene horneada al lado del binario
extern std::string w3dPath;

// ---- AVISOS al usuario: el editor = el cartelito de la esquina; un juego = el log (ver tambien W3dNotice.h)
void Notify(const std::string& msg, bool error);

// ---- la BARRA DE PROGRESO de una carga larga: el editor = un popup; un juego = nada
void ProgressStart(const std::string& msg);
void ProgressUpdate(float frac);
void ProgressEnd();

// ---- EL PLAY: en el editor hay una partida cargada o no (SimGame); un juego corre SIEMPRE
bool SimActive();
bool W3dGameRunning();

// ---- MODELOS EXTERNOS (GLB/glTF/FBX): los importadores son del editor. Un proyecto GUARDADO no los
//      necesita (al guardar, la geometria queda en el formato propio); un juego avisa y sigue
bool ImportModelByExtension(const std::string& path);

// ---- NAVEGADOR: el archivo recien escrito (un export) se descarga a la PC (fuera del navegador no existe)
#ifdef __EMSCRIPTEN__
void W3dDownloadIfWeb(const std::string& vfsPath);
#endif

// ---- la VISTA PREVIA de un video del elemento Video2D: el editor extrae cuadros con ffmpeg (io/Video2DCache.cpp);
//      un juego no tiene preview (NULL: el elemento dibuja su recuadro) -el video de verdad lo reproduce aparte-
struct VideoPreview {
    unsigned* frames;   // ids de textura GL (NULL si no se pudo)
    int numFrames;
    int w, h;           // tamano de los frames extraidos
    int widthReal, heightReal;   // el tamano REAL del video (para el elemento)
    float fps;
};
const VideoPreview* Video2DPreview(const std::string& path);   // NULL = sin preview
unsigned W3dMsNow();   // milisegundos monotonicos (la animacion de la preview)

// ---- el MIPMAPPING por defecto cuando el proyecto no lo declara: el del editor es una opcion de Ajustes
//      (cfg.mipmaps); un juego usa el de fabrica (prendido)
bool W3dHostMipmapsDefault();

// ---- el DESHACER de las operaciones de malla (edit/MeshEdit.cpp). Un juego no deshace: no-op
void UndoCaptureMeshGeo(Mesh* m);
void UndoCaptureRenames(const std::vector<W3dRenameDest>& destinations);
void UndoMergeLast(int n);
bool UndoMoveLayerMesh(Mesh* m, int layer, int i, int j);
void UndoModAdd(Mesh* m, int type);
void UndoModRemove(Mesh* m);
void UndoModMove(Mesh* m, int dir);
void UndoModEmpty(Mesh* m);

#endif // W3D_HOST_H
