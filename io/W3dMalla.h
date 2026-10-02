#ifndef W3D_MALLA_H
#define W3D_MALLA_H

// ============================================================================
//  .w3dm - EL FORMATO DE GEOMETRIA PROPIO DE WHISK3D (version 1 del lexico)
//
//  Texto plano por LINEAS, legible y editable a mano, pensado para que un N95 lo
//  lea rapido y sin fragmentar el heap. Reemplaza al GLB como formato de
//  GUARDADO de mallas (el GLB queda para importar/exportar a otros programas).
//
//  EL LEXICO, que es lo que sostiene todo. Se mira el PRIMER CARACTER de cada
//  linea y con eso alcanza:
//
//      'A'..'Z'   ABRE UN BLOQUE. Se cierra con una linea "END" en la columna 0.
//      'a'..'z'   DIRECTIVA de una sola linea: clave valor(es).
//      ' ' o TAB  DATO (payload) del bloque abierto.
//      '#'        comentario. Linea vacia: se ignora.
//
//  Ningun dato empieza con letra, asi que NUNCA hay ambiguedad y NUNCA hace
//  falta contar bytes para saber donde termina un bloque. Esa es la propiedad
//  que permite que un lector de hoy saltee (y PRESERVE) un bloque inventado
//  dentro de tres anios sin entender absolutamente nada de el.
//
//  CABECERA DE BLOQUE:
//      <NOMBRE> <cantidad> <dominio> [clave=valor ...] [nombre=<resto de linea>]
//    - NOMBRE oficial [A-Z][A-Z0-9]*; experimental X.<quien>.<que> (X.PC2.PLIEGUE)
//    - cantidad: cuantos ELEMENTOS trae -> se reserva la memoria DE UNA SOLA VEZ
//    - dominio: punto | esquina | cara | arista | global (vocabulario CERRADO)
//    - nombre= va SIEMPRE ULTIMO y toma el RESTO de la linea (asi un nombre con
//      espacios no necesita comillas ni escapes)
//
//  EL "END" ES TOLERANTE: se le recortan los blancos de los dos bordes y se
//  acepta en cualquier caja ("END", "END ", "end"). Un formato que se vende como
//  editable a mano no puede tener una trampa en la que un espacio INVISIBLE deje
//  el bloque abierto y se coma el resto del archivo.
//
//  LOS NOMBRES VUELVEN EXACTOS. Un nombre (el de la malla, el de una capa, el de
//  un grupo, el de un mesh part, el de un material) va TAL CUAL hasta el fin de
//  la linea, espacios de los bordes incluidos: son VINCULOS contra el
//  proyecto.json, y recortarlos rompe el binding en silencio. El unico caracter
//  que no se puede representar es el SALTO DE LINEA: el escritor lo cambia por un
//  espacio y AVISA.
//
//  EL CHOQUE CONTRA EL "END", Y SU ESCAPE. PARTS es el UNICO bloque cuyo PAYLOAD
//  es un nombre pelado (los demas o son numeros, o llevan el nombre en la
//  cabecera con nombre=, o lo preceden con un indice). Un mesh part llamado "END"
//  emitia la linea " END", el END tolerante la recortaba, el bloque se cerraba
//  ahi y el "END" verdadero que venia despues se leia como una cabecera mal
//  formada que se comia el bloque PARTMAT entero: la malla perdia las partes Y el
//  vinculo a sus materiales. Por eso el payload de PARTS lleva ESCAPE: si el
//  nombre recortado seria un END, o si ya empieza con '\', el escritor le antepone
//  UN '\' y el lector le saca UNO. Es lossless, no ensancha el bloque (sigue
//  siendo ancho 1) y se explica en una linea al que edita a mano. Un bloque NUEVO
//  con nombres en el payload tiene que hacer lo mismo (o poner el nombre en la
//  cabecera, que es lo preferible).
//
//  LA GEOMETRIA SUELTA NO SE SUELDA. El bloque V dedupea posiciones (por eso un
//  cubo tiene 8 puntos y no 24), pero cada vertice/extremo SUELTO se lleva su
//  punto propio aunque otro este parado exactamente ahi: dos vertices sueltos
//  encimados son DOS, el editor no los une, y el archivo tampoco puede.
//
//  DOS ESPACIOS DE INDICES Y NADA MAS: 'punto' (posicion 3D unica, o sea el
//  vertice que el usuario CUENTA: un cubo tiene 8) y 'esquina' (esquina de cara,
//  numeradas recorriendo el bloque F). 'cara' y 'arista' se derivan. TODOS los
//  indices arrancan en 0 (el .obj usa base 1 y eso es una fuente clasica de
//  bugs; aca el lector escribe DIRECTO en el array destino sin sumar ni restar).
//
//  EL VERTICE DE RENDER NO ESTA EN EL ARCHIVO. En memoria MeshFace::idx indexa
//  el array de render (un vertice por combinacion unica de pos+uv+normal+color),
//  pero ese array NO se guarda: el merge que lo produce es una FUNCION PURA Y
//  DETERMINISTA de posicion + UV activa + normal por esquina + color activo, con
//  orden de PRIMERA APARICION recorriendo las caras. Los cuatro ingredientes son
//  exactamente lo que el archivo guarda (V, la UV activa, NRM y la COL activa) y
//  el orden lo fija el bloque F -> se reproduce BIT A BIT sin guardarlo. El
//  bloque opcional RVMAP es solo un cache/ancla para las vertex animations.
//
//  QUE NO VA ACA: transform, jerarquia, materiales, modificadores, armatures (3D
//  y 2D) y curvas de animacion viven en el proyecto.json; los keyframes de
//  vertices en su blob binario. En el .w3dm va TODO lo que sobrevive a copiar la
//  malla a otro proyecto, y nada mas.
//
//  C++03 puro. Este archivo entra en la build del JUEGO COMPILADO (Linux/
//  Android/web/Symbian), asi que NO puede llamar a nada que viva en main/ (el
//  editor): el lector deja la malla con sus DATOS y el que llama cierra con
//  ReagruparMeshParts() + CalcularBordes() + AplicarCapasAlRender().
//
//  EL MISMO DATO EN BINARIO: el guardado escribe por defecto la malla en .w3db
//  (W3dMallaBin.h), que trae ese cierre YA HECHO y abre con un memcpy. Este texto
//  queda para quien quiera diffs legibles (opcion del proyecto "formatoMallas":
//  "texto") y para los .w3d de antes. Los dos escritores comparten las piezas del
//  final de este archivo: la misma malla vuelve igual de cualquiera de los dos.
// ============================================================================

#include <string>
#include <vector>
#include <map>
#include <set>
#include <utility>
#include <cstddef>

class Mesh;

// resultado de una lectura: todo lo que el que llama necesita saber y no cabe en
// la malla misma.
struct W3dMallaInfo {
    // un renglon por problema, con el numero de linea. NUNCA se falla en
    // silencio: la garantia que damos es "no se corrompe y siempre se avisa",
    // NO "nunca se pierde".
    //
    // ACOTADA (kW3dmMaxAvisos): un .w3dm corrupto de 2 MB puede tener 200.000
    // lineas malas y esta lista era LO UNICO del lector sin techo (200.001
    // strings = decenas de MB de RSS por un archivo roto; en un N95 eso es el
    // bad_alloc). Se guardan los PRIMEROS y se cuentan los demas en
    // 'avisosDeMas': el ultimo renglon lo dice, asi que el techo no esconde nada.
    std::vector<std::string> avisos;
    int  avisosDeMas;  // avisos que NO entraron en la lista (0 = estan todos)
    // nombre del MATERIAL de cada mesh part ("" = ninguno). El material vive en
    // la escena y se referencia POR NOMBRE, nunca por indice: el Core no puede
    // resolver el puntero, lo hace el que llama.
    std::vector<std::string> materiales;
    int  rverts;       // directiva "rverts" del archivo (-1 = ausente)
    int  rvertsReales; // vertices de render que salieron del merge (-1 = no se cargo)
    bool truncado;     // se acabo el archivo con un bloque abierto, o FALTA V/F
    bool soloLectura;  // "requiere" nombra un bloque que este Whisk3D no conoce
    W3dMallaInfo() : avisosDeMas(0), rverts(-1), rvertsReales(-1), truncado(false), soloLectura(false) {}
};

// techo de la lista de avisos (ver W3dMallaInfo::avisos)
enum { kW3dmMaxAvisos = 200 };

// escribe la malla completa en 'out' (se limpia primero). false = malla NULL.
// DETERMINISTA: la misma malla da SIEMPRE los mismos bytes (round-trip estable).
// 'avisos' (opcional) recibe un renglon por CADA cosa que no sale entera al
// archivo. La perdida SILENCIOSA tambien es un bug: si algo no sobrevive, se dice.
// Hoy son cinco:
//   - un bloque de una version mas nueva que no se pudo conservar (cambio la topologia);
//   - una capa UV/COL que no mide lo que la topologia dice (se saltea, y el indice de
//     la capa ACTIVA se corrige a la lista realmente escrita);
//   - un peso de vertex group que cae en un punto ya ocupado (dos control-points en la
//     MISMA posicion: en el archivo son UN punto y gana el primero);
//   - un borde SHARP o una costura SEAM cuya clave de POSICION ya no cae en ningun
//     punto de la malla (la clave quedo stale porque se movio un vertice);
//   - un nombre con salto de linea (partiria el archivo en dos): se sanea a un espacio.
//
// EL QUE LLAMA TIENE QUE MOSTRAR LOS AVISOS AL USUARIO, no solo loguearlos: "siempre
// se avisa" no se cumple con una linea en un log que nadie abre.
//
// DEVUELVE false (y NO escribe nada) si la malla esta bajo el freno de mano
// "requiere": ver W3dMallaBloqueQueFalta.
//
// 'anclarRender': si el archivo lleva el ancla de los render-verts (directiva rverts + bloque
// RVMAP, el orden de memoria que indexan las vertex anims). -1 (default) = solo si ESTA malla
// tiene vertex anims; 1 = siempre (una malla 3D COMPARTIDA, MallaRecurso.h, cuyo recurso usa
// otro objeto con vertex anims: el orden es de todos sus usuarios); 0 = nunca.
bool W3dMallaEscribir(const Mesh* m, std::string& out, std::vector<std::string>* avisos = 0,
                      int anclarRender = -1);

// EL FRENO DE MANO. "" = la malla se puede guardar. Si no, devuelve el NOMBRE del
// bloque que el archivo declaro obligatorio con "requiere" y esta version NO
// entiende: guardar encima degradaria el archivo en silencio, que es exactamente
// lo que esa directiva existe para impedir. Se consulta ANTES de escribir para
// poder decirle al usuario CUAL es el bloque; W3dMallaEscribir lo vuelve a mirar
// por su cuenta para que nadie pueda saltearse el freno.
std::string W3dMallaBloqueQueFalta(const Mesh* m);

// lee un .w3dm sobre 'm'. Reconstruye vertex/normals/uv/vertexColor/faces3d +
// todas las capas. NO arma el index buffer ni los bordes: el que llama cierra
// con ReagruparMeshParts() + CalcularBordes() + AplicarCapasAlRender().
// false = ni siquiera es un .w3dm, o pide una version del lexico mas nueva, o NO
// TRAE los bloques V y F que el propio formato declara obligatorios ("requiere V F").
// En los tres casos la malla que entro NO SE TOCA: un archivo cortado justo en un
// END (un zip copiado a medias) tiene que dejar la malla como estaba y avisar, jamas
// vaciarla y devolver true.
bool W3dMallaLeer(const char* datos, size_t n, Mesh* m, W3dMallaInfo* info);
bool W3dMallaLeerTexto(const std::string& txt, Mesh* m, W3dMallaInfo* info);

// true si el nombre es un bloque que ESTA version entiende (lo usa "requiere").
bool W3dMallaBloqueConocido(const std::string& nombre);

// ============================================================================
//  PIEZAS DEL ESCRITOR QUE COMPARTE EL .w3db (io/W3dMallaBin.h, la malla BINARIA)
//
//  El .w3dm (texto) y el .w3db (binario) guardan LA MISMA malla: que es un PUNTO,
//  que capas salen, que marcas siguen siendo una arista y como viajan los pesos se
//  decide UNA sola vez, aca, con el mismo codigo. Asi una malla guardada en texto y
//  la misma guardada en binario vuelven IDENTICAS al abrirlas (lo asserta el
//  harness con mallafoto / mallafotoigual). Los avisos son los mismos tambien.
// ============================================================================

// LOS PUNTOS de la malla (el vertice que el usuario cuenta: un cubo tiene 8). Dos
// render-verts son el mismo punto si tienen la misma POSICION (bytes) y los mismos
// DATOS DE PUNTO (pesos de cada vertex group, deltas de cada shape key); la geometria
// SUELTA lleva siempre su punto propio. Guardar nunca suelda lo que la malla no tiene
// soldado: esta es la identidad que el lector devuelve en vertCtrlPoint/posRep.
struct W3dMallaPuntos {
    int nV;                            // render-verts (0 si la malla no tiene vertex[])
    int nP;                            // puntos
    std::vector<int>   rvAPunto;       // render-vert -> punto
    std::vector<float> pts;            // xyz de cada punto (3 por punto)
    // posicion (12 bytes) -> TODOS los puntos parados ahi, de menor a mayor (sharp/seam
    // se indexan por posicion)
    std::map<std::string, std::vector<int> > porPos;
    W3dMallaPuntos() : nV(0), nP(0) {}
};
void W3dMallaCalcularPuntos(const Mesh* m, W3dMallaPuntos& out);

// AGRUPA la identidad de los puntos (vertCtrlPoint) de una malla SKINNEADA ya cargada cuyo
// .w3db vino con ctrl casi-identidad ("sopa de triangulos": muchos render-verts coincidentes
// sin colapsar). Fusiona los que comparten posicion + pesos + deltas de forma en un punto unico
// y REMAPEA los vertex groups y las shape keys a ese espacio -> el skinning calcula 1 vez por
// punto real y escalea (LOSSLESS; el G-Man pasa de 10170 a ~1695 puntos = 6x menos skinning).
// No toca el render (triangulos/UV). Si algun merge diera distinto peso/delta (no deberia: la
// firma lo separa) ABORTA sin tocar nada. Idempotente: si ya esta agrupada, no hace nada.
void W3dReagruparPuntosSkin(Mesh* m);

// QUE CAPAS SALEN al archivo. Una capa UV/color que no mide lo que la topologia dice
// (nC esquinas) no sale y se avisa; el indice de la ACTIVA es el de la lista ESCRITA.
// Capas LAZY (la malla todavia no las materializo): sale la version derivada del
// render, con los nombres que les pondria PoblarCapas ("UVMap" / "Col").
struct W3dMallaCapasSalen {
    std::vector<size_t> uv, col;   // indices en uvMaps / colorLayers, en su orden
    int  uvActiva, colActiva;      // la ACTIVA dentro de la lista escrita (-1 = ninguna)
    bool uvDerivada, colDerivada;  // no hay capas: sale la derivada del render
    W3dMallaCapasSalen() : uvActiva(-1), colActiva(-1), uvDerivada(false), colDerivada(false) {}
};
void W3dMallaElegirCapas(const Mesh* m, int nC, W3dMallaCapasSalen& out, std::vector<std::string>* avisos);

// las ARISTAS REALES de la malla: pares de PUNTOS (a < b), sin repetir, de las caras + la
// geometria suelta. Validan SHARP/SEAM y cuentan el dominio 'arista' de los bloques ajenos.
void W3dMallaAristasReales(const Mesh* m, const W3dMallaPuntos& P, std::set<std::pair<int, int> >& out);

// las marcas SHARP (seam=false) o SEAM (seam=true) que siguen siendo una arista real, como
// PARES DE PUNTOS ordenados y sin repetir. Lo que se perdio (la posicion ya no cae en ningun
// punto, o no hay arista entre esos puntos) se avisa. Respeta el invariante de POSE.
void W3dMallaMarcasValidas(const Mesh* m, const W3dMallaPuntos& P, const std::set<std::pair<int, int> >& aristas,
                           bool seam, std::vector<std::pair<int, int> >& pares, std::vector<std::string>* avisos);

// control-point -> PRIMER render-vert que lo usa (vacio si vertCtrlPoint no cubre la malla)
void W3dMallaRepDeCp(const Mesh* m, std::map<int, int>& out);
// los pesos del vertex group g POR PUNTO, en orden de aparicion y sin repetir punto (el
// primero gana; dos control-points distintos en el mismo punto con OTRO peso se avisan).
// false = el grupo es NULL (no sale).
bool W3dMallaGrupoPorPunto(const Mesh* m, const W3dMallaPuntos& P, const std::map<int, int>& repDeCp,
                           size_t g, std::vector<int>& pi, std::vector<float>& pw, std::vector<std::string>* avisos);
// los deltas de la shape key k POR PUNTO (3 floats por punto), mismo pasaje que los grupos
void W3dMallaFormaPorPunto(const Mesh* m, const W3dMallaPuntos& P, const std::map<int, int>& repDeCp,
                           size_t k, std::vector<int>& pi, std::vector<float>& pd);
// el UV group g POR ESQUINA: (esquina, peso) de las esquinas con peso > 0. false = NULL.
bool W3dMallaUVGrupoPorEsquina(const Mesh* m, size_t g, std::vector<int>& ci, std::vector<float>& cw);

// un NOMBRE listo para el archivo: el unico caracter que el formato no puede representar
// es el SALTO DE LINEA; se cambia por un espacio y se avisa ('que' dice de quien es).
std::string W3dMallaNombreSano(const std::string& n, std::vector<std::string>* avisos, const char* que);

#endif
