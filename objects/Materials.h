#ifndef MATERIALS_H
#define MATERIALS_H

#ifdef _WIN32
    #define NOMINMAX
#ifndef W3D_SYMBIAN
    #include <windows.h>
#endif
#endif

#include <vector>
#include <string>
#ifdef W3D_SYMBIAN
    #include <GLES/gl.h>
#else
    #include <GL/gl.h>
#endif
#include "Textures.h"

// Declaración adelantada de Material
class Material;

// Vector global de materiales
extern std::vector<Material*> Materials;
extern Material* MaterialDefecto;

// ===================================================
//  CIERRE DE PROYECTO: podar los materiales del proyecto
//
//  `Materials` es un registro GLOBAL DE PROCESO y el constructor de Material es
//  su unico punto de alta. Hasta hoy ReiniciarEscena() no lo tocaba: al reabrir
//  un proyecto en el mismo proceso los materiales del anterior seguian vivos, el
//  ctor los veia como homonimos y uniquificaba -> TODO el proyecto volvia con
//  sufijo ".001" (y si despues se guardaba, el sufijo quedaba en el disco).
//
//  Funciona igual que la marca base de las texturas (TexturasFijarBase): lo que
//  se crea ANTES de la marca es del EDITOR y no se poda nunca.
// ===================================================
void MaterialesFijarBase();   // la fija el arranque de la UI, una sola vez
int  MaterialesBase();        // cuantos materiales quedan fijos (los del editor)
// borra los materiales DEL PROYECTO (los de arriba de la marca) y los saca de la
// lista; devuelve cuantos. Lo llama ReiniciarEscena DESPUES de destruir el arbol:
// ninguna malla puede seguir apuntando a un Material liberado.
int  MaterialesLiberarEscena();

// ===================================================
//  PURGAR un material HUERFANO (el outliner por recursos, "Purge orphans")
//
//  Sale de `Materials` (no se guarda mas ni aparece en los selectores) pero NO se
//  libera: pasos de undo viejos (el material de un mesh part, un snapshot de la
//  geometria) pueden guardar su puntero y volver a ponerlo en una malla. Queda en
//  el CEMENTERIO hasta que se cierre el proyecto (MaterialesLiberarEscena lo
//  libera junto con los demas). Si un undo lo vuelve a usar, MaterialRevivir lo
//  devuelve a la lista (con un nombre libre).
// ===================================================
// false = no es un material del proyecto (base de la UI, el por defecto, o ya purgado)
bool MaterialPurgar(Material* m);
// true = estaba en el cementerio y volvio a `Materials`
bool MaterialRevivir(Material* m);
const std::vector<Material*>& MaterialesPurgados();

// ===================================================
// Clase Material
// ===================================================
// capa de textura EXTRA (encima de la textura base del material): su textura, con QUE UV se dibuja y como se
// MEZCLA con lo de abajo. Render por MULTI-PASS (GL 1.1, portable PC+N95): cada capa es un draw mas sobre la
// misma superficie, con la textura PURA (sin luz ni color de vertice: eso ya lo aplico la base).
//   uvMapa: indice de la capa UV de la malla con la que se dibuja (Mesh::uvMaps / Mesh::UVDeCapa). 0 = la de
//           siempre; 1 = la segunda capa (ej. el unwrap propio de un lightmap horneado, TEXCOORD_1 del glTF).
//   blend:  W3dCapaMezcla. "Multiply 2x" es el modo de los lightmaps con overbright (dst * src * 2).
enum W3dCapaMezcla {
    W3D_CAPA_MIX = 0,        // alpha encima (SRC_ALPHA, ONE_MINUS_SRC_ALPHA)
    W3D_CAPA_MULTIPLY = 1,   // DST_COLOR, ZERO (oscurece: sombras, lightmaps)
    W3D_CAPA_ADD = 2,        // ONE, ONE (aclara: brillos, luces)
    W3D_CAPA_MULTIPLY2X = 3, // DST_COLOR, SRC_COLOR = 2 * dst * src (lightmap con overbright hasta 2.0)
    W3D_CAPA_SUBTRACT = 4,   // resta (REVERSE_SUBTRACT en PC; aprox en GL ES 1.1)
    W3D_CAPA_SCREEN = 5,     // ONE, ONE_MINUS_SRC_COLOR (aclara suave)
    W3D_CAPA_MEZCLAS = 6     // (cantidad; dejalo ultimo)
};
const char* W3dCapaMezclaNombre(int m);   // "Mix", "Multiply", ... (panel, harness, logs)
class TexLayer {
    public:
        Texture* tex;
        int blend;   // W3dCapaMezcla
        int uvMapa;  // capa UV de la malla (0 = la base)
        bool on;
        TexLayer() : tex(NULL), blend(0), uvMapa(0), on(true) {}
};

class Material {
    public:
        // defaults en el constructor (Materials.cpp): los inicializadores
        // en la clase son C++11 y esto compila tambien en RVCT (C++03)
        bool textureOn;   // prender/apagar la textura (checkbox de la UI)
        bool filtrado;    // filtrado de textura (lineal) o pixel perfect
        bool transparent;
        bool vertexColor;
        bool lighting;
        bool repeat;
        bool uv8bit;
        bool culling;
        bool depth_test;
        // FONDO (el cielo): se dibuja ULTIMO de los opacos, clavado al plano lejano
        // (DepthRange 1..1 + LEQUAL, sin escribir z): solo pinta los pixeles que ningun
        // opaco toco -> cero overdraw del cielo. En el .mtl es la palabra `FONDO`.
        bool fondo;
        // ---- DECAL / orden de pasada (calcomanias: sombras, blobs, manchas) ----
        // Una calcomania se dibuja PEGADA a una superficie que ya escribio z. Si escribe z
        // ella tambien queda coplanar y pelea (z-fighting). La receta es la misma que usa
        // Mirror.cpp para la mascara del agua: NO escribir z + correrla hacia el ojo con
        // glPolygonOffset. En el .mtl basta la palabra `decal`.
        bool depth_write;  // escribir el z-buffer (default true). false = calcomania
        float depth_bias;  // unidades de glPolygonOffset (default 0 = sin offset). NEGATIVO = hacia el ojo.
                           // -4 anda en PC (z de 24 bits). En el N95 (z de 16 bits) hace falta -8..-16:
                           // el render lo multiplica x4 en Symbian, asi que el archivo guarda -4 igual.
        // SESGO DE PROFUNDIDAD EN METROS (espacio de la camara): la malla se dibuja como si estuviera N metros mas LEJOS
        // (positivo) o mas CERCA (negativo) para el z-buffer, sin moverse en pantalla. A diferencia de depth_bias
        // (unidades del z-buffer, que valen distinto segun la distancia), esto pesa lo mismo cerca y lejos: un piso a
        // +0.05 pierde los empates de 5 cm contra los autos y sombras que se apoyan en el (como un arcade sin z-buffer
        // que dibuja el auto despues de la pista). 0 = nada.
        float sesgo_metros;
        int  orden_pasada; // 0 = opaco, 1 = decal, 2 = transparente. Dentro de una MALLA los grupos de
                           // material se dibujan en este orden (opaco -> decal -> transparente). Entre
                           // objetos distintos el orden lo sigue decidiendo el arbol / la Collection.
        int  mezcla;       // modo de mezcla cuando transparent = true. 0 = alpha (default, compatible
                           // con todo lo guardado hasta hoy); el resto son los codigos de gfx::Mezcla
                           // (2 = MezclaAdd = ADITIVO: GL_ONE/GL_ONE, aclara y el negro es invisible).
        float alphaTest;   // RECORTE POR ALFA (GL_ALPHA_TEST, fijo en GL ES 1.1): 0 = apagado; > 0 = se descartan los
                           // texels con alfa <= ref. Rejas, alambrados, hojas: SIN blend, con z y en la pasada opaca,
                           // asi no dependen del orden de dibujo (con blend el agujero escribia z y tapaba lo de atras).
        // TRAZADO DE RAYOS (editor, render/RayTracer): rugosidad 0..1 (0 = espejo, 1 = mate) y metalico 0..1
        // (0 = difuso; 1 = metal: refleja la escena teñido por su color). Metalico 1 + rugosidad 0 = espejo.
        float rtRugosidad;
        float rtMetalico;
        bool chrome;      // "Reflection": reflejo de entorno (env-map). On/off; el MODO lo elige reflectMode.
        int  reflectMode; // 0 = Matcap (normal-del-ojo, matriz de textura, HARDWARE en PC y N95; rapido)
                          // 1 = Sphere Map exacto (GL_SPHERE_MAP: HARDWARE en PC via texgen, SOFTWARE en N95)
                          // 2 = Equirectangular 360 (SOFTWARE, calidad). (VGP exacto por HW = futuro, falta API de IMG)
        bool normalMap;   // NORMAL MAPPING (DOT3): la textura 'normalTexture' perturba la normal por pixel.
                          // Multi-pass: base * (N.L). Excluyente con chrome (mismo combiner). Portable PC+N95.
        Texture* normalTexture; // el normal map (RGB = normal en tangent-space)
        // MEZCLA DE TEXTURAS en una pasada (gfx::MezclaTexturas, multitextura): 'texturaB' se interpola con la textura
        // base en el combiner de la unidad 1: fragmento = B * mezclaB + base (con su luz) * (1 - mezclaB). B sale plana
        // (sin luz). Claves del .w3d: "texturaB" (ruta, como "normalTextura") y "mezclaB" (0..1; con 0 la unidad 1 ni
        // se prende). Lua: setTexturaB / setMezclaTextura / mezclaTextura; panel: "Texture B" / "Texture mix";
        // rutinas: el paso "Texture mix". No se combina con el reflejo ni con el normal map (van por el combiner)
        Texture* texturaB;
        float    mezclaB;
        // ---- LINEAS (aristas de la malla dibujadas encima del relleno) ----
        // Con lineas = true, cada grupo que use este material dibuja ADEMAS sus
        // ARISTAS (los 3 lados de cada triangulo, deduplicados) con glLineWidth
        // = grosorLinea, en pixeles, respetando el color y la textura del
        // material. Si la malla es una TIRA degenerada (triangulos de area cero:
        // un camino de vertices), el relleno no pinta nada y las aristas dibujan
        // el CAMINO de la polilinea: una soga, un cable, un trazo.
        // .mtl: `lineas [grosor]` ; JSON del proyecto: "lineas" + "grosorLinea".
        bool  lineas;      // default false
        float grosorLinea; // en px (default 1). El tope real lo pone el driver.
        int interpolacion;
        Texture* texture;
        std::vector<TexLayer> capas; // capas de textura EXTRA encima de 'texture' (multi-pass)
        float diffuse[4];
        float specular[4];
        float emission[4];
        float ambient[4]; // reflectancia de la luz ambiente del material
        float shininess;  // brillo especular (exponente)
        std::string name;
        // CARPETA cosmetica del outliner por recursos ("Piel/Enemigos"; "" = la raiz). No
        // afecta al render ni al runtime: solo ordena la vista de materiales del editor.
        std::string carpeta;
        // LA LIBRERIA externa de la que es ("" = del proyecto). Un material de libreria vive en SU espacio de
        // nombres (el nombre lleva el prefijo: "Personajes/Piel"), es de SOLO LECTURA y no se guarda en el
        // proyecto (el que lo nombra guarda solo su nombre). Ver main/io/Librerias.h.
        std::string libreria;

        Material(const std::string& nombre, bool MaterialDefecto = false, bool TieneVertexColor = false);
        ~Material();

        // LA PUERTA del rename: uniquifica en el espacio GLOBAL de materiales.
        void SetName(const std::string& nombre);
};

// ===================================================
// Funciones auxiliares
// ===================================================
Material* BuscarMaterialPorNombre(const std::string& name);
// nombre libre en el espacio GLOBAL de materiales ('excepto' conserva el suyo).
std::string MaterialNombreLibre(const std::string& base, const Material* excepto);

// ===================================================
//  LA RECETA DE LA CALCOMANIA, EN UN SOLO LUGAR
//
//  "decal" no es un flag: son CUATRO campos que van juntos (transparente + no
//  escribir z + polygon offset hacia el ojo + orden de pasada 1). La palabra
//  `decal` del .mtl, el tilde "Decal" del panel de materiales y el assert
//  `matflags <mat> decal` del harness describian LO MISMO en tres lugares
//  distintos: ahora los tres llaman aca.
//    W3dMaterialEsDecal      -> el material cumple la receta entera
//    W3dMaterialAplicarDecal -> la aplica (on) o la deshace (off). 'unidades' es el
//                               polygon offset; 0 usa el default (-4, que anda en PC
//                               con z de 24 bits; en el N95 el render lo multiplica x4).
// ===================================================
bool W3dMaterialEsDecal(const Material* m);
void W3dMaterialAplicarDecal(Material* m, bool on, float unidades = 0.0f);

// ===================================================
//  MATERIAL "LUZ": EL QUE MODULA EL FRAMEBUFFER Y POR LO TANTO NO TAPA NADA
//
//  Una mezcla ADITIVA (ONE/ONE), SUSTRACTIVA, MULTIPLICATIVA o SCREEN no
//  reemplaza el color del fondo: lo SUMA, lo RESTA o lo escala. Geometria asi
//  es LUZ, no cuerpo: un destello, una chispa, un halo, un rayo, una estela.
//  Nunca puede OCULTAR lo que tiene detras -- y por lo tanto NO TIENE NADA QUE
//  ESCRIBIR EN EL Z-BUFFER.
//
//  El bug que motivo esta regla (reporte del dueno, con captura): las chispas
//  del juego demo son una malla de quads con material ADITIVO que vive ARRIBA
//  en el arbol; el personaje al que envuelven se dibuja MILES de lineas mas
//  abajo y un poco mas lejos. Como la chispa escribia z, el personaje perdia el
//  z-test justo en el rectangulo del quad y quedaba un AGUJERO con la forma de
//  la chispa, por el que se veia el escenario del fondo. (Prueba por pixeles:
//  tools/pruebas/prueba_particulas_z.w3s, estacion A.)
//
//  La regla vale en el RENDER, no en los campos del material: `depth_write`
//  sigue siendo lo que el usuario/el archivo pidieron (el round-trip del .w3d
//  no cambia), y la mezcla ALFA comun -- que SI compone como un cuerpo y suele
//  usarse de recorte -- sigue escribiendo z como siempre.
// ===================================================
bool W3dMaterialEsLuz(const Material* m);

class AnimatedMaterial {
    public:
        std::vector<Material*> targets;    // targets → materiales afectados
        std::vector<Texture*> frameTextures; // textures → cada frame de la animación
        std::vector<int> frameDurations;     // speeds → duración por frame (en ticks)

        int frameIndex;     // índice del frame actual (0)
        float tickCounter;  // tiempo acumulado en el frame actual, en TICKS de 120 Hz (el loop viejo
                            // corria a ~120 updates/seg: el contenido esta calibrado asi)

        AnimatedMaterial() : frameIndex(0), tickCounter(-1.0f) {}
        void Update(float dtSeg);   // dt REAL del frame: la velocidad no depende de los fps
};

extern std::vector<AnimatedMaterial*> AnimatedMaterials;
extern void UpdateAnimatedMaterials(float dtSeg = 1.0f / 60.0f);

// true si hay un material animado activo (animacion EN PLAY). Lo usa el render
// event-driven del loop (PC/Symbian) para seguir redibujando mientras algo se
// mueve sin input. Vive aca (Materials.cpp se compila en los 4 OS); el sistema
// de vertex-animation es solo de PC, asi que el loop de PC ademas chequea
// VertexAnimationActives por su lado.
bool HayAnimacionActiva();

#endif