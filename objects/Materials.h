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
// Clase Material
// ===================================================
// capa de textura EXTRA (encima de la textura base del material): su textura + como se MEZCLA con lo de
// abajo. Comparten las UV del modelo (UV por capa = TODO). Render por MULTI-PASS (GL 1.1, portable PC+N95).
class TexLayer {
    public:
        Texture* tex;
        int blend;  // 0 = Mix (alpha encima), 1 = Multiply, 2 = Add
        bool on;
        TexLayer() : tex(NULL), blend(0), on(true) {}
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
        int  orden_pasada; // 0 = opaco, 1 = decal, 2 = transparente. Dentro de una MALLA los grupos de
                           // material se dibujan en este orden (opaco -> decal -> transparente). Entre
                           // objetos distintos el orden lo sigue decidiendo el arbol / la Collection.
        int  mezcla;       // modo de mezcla cuando transparent = true. 0 = alpha (default, compatible
                           // con todo lo guardado hasta hoy); el resto son los codigos de gfx::Mezcla
                           // (2 = MezclaAdd = ADITIVO: GL_ONE/GL_ONE, aclara y el negro es invisible).
        bool chrome;      // "Reflection": reflejo de entorno (env-map). On/off; el MODO lo elige reflectMode.
        int  reflectMode; // 0 = Matcap (normal-del-ojo, matriz de textura, HARDWARE en PC y N95; rapido)
                          // 1 = Sphere Map exacto (GL_SPHERE_MAP: HARDWARE en PC via texgen, SOFTWARE en N95)
                          // 2 = Equirectangular 360 (SOFTWARE, calidad). (VGP exacto por HW = futuro, falta API de IMG)
        bool normalMap;   // NORMAL MAPPING (DOT3): la textura 'normalTexture' perturba la normal por pixel.
                          // Multi-pass: base * (N.L). Excluyente con chrome (mismo combiner). Portable PC+N95.
        Texture* normalTexture; // el normal map (RGB = normal en tangent-space)
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