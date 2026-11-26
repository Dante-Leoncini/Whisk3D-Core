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
extern Material* MaterialDefault;
// un material que CREA el editor (uno nuevo, el material por defecto) nace con el sombreado POR PIXEL si el backend tiene
// shaders (Vulkan, OpenGL ES 2.0, la web: w3dEngine::ShadingPerPixelSupported); sin shaders (el OpenGL fijo, el
// N95), por vertice. Devuelve el mismo material. (El constructor sigue en "por vertice": es lo que significa un
// material de un .w3d sin "luzPorPixel", y abrir un proyecto viejo no tiene que cambiarle el aspecto)
Material* FactoryMaterial(Material* m);

// ===================================================
//  CIERRE DE PROYECTO: podar los materiales del proyecto
//
//  `Materials` es un registro GLOBAL DE PROCESO y el constructor de Material es
//  su unico punto de alta. Hasta hoy RestartScene() no lo tocaba: al reabrir
//  un proyecto en el mismo proceso los materiales del anterior seguian vivos, el
//  ctor los veia como homonimos y uniquificaba -> TODO el proyecto volvia con
//  sufijo ".001" (y si despues se guardaba, el sufijo quedaba en el disco).
//
//  Funciona igual que la marca base de las texturas (TexturesSetBase): lo que
//  se crea ANTES de la marca es del EDITOR y no se poda nunca.
// ===================================================
void MaterialsSetBase();   // la fija el arranque de la UI, una sola vez
int  MaterialsBase();      // cuantos materiales quedan fijos (los del editor)
// borra los materiales DEL PROYECTO (los de arriba de la marca) y los saca de la
// lista; devuelve cuantos. Lo llama RestartScene DESPUES de destruir el arbol:
// ninguna malla puede seguir apuntando a un Material liberado.
int  MaterialsFreeScene();

// ===================================================
//  PURGAR un material HUERFANO (el outliner por recursos, "Purge orphans")
//
//  Sale de `Materials` (no se guarda mas ni aparece en los selectores) pero NO se
//  libera: pasos de undo viejos (el material de un mesh part, un snapshot de la
//  geometria) pueden guardar su puntero y volver a ponerlo en una malla. Queda en
//  el CEMENTERIO hasta que se cierre el proyecto (MaterialsFreeScene lo
//  libera junto con los demas). Si un undo lo vuelve a usar, MaterialRevive lo
//  devuelve a la lista (con un nombre libre).
// ===================================================
// false = no es un material del proyecto (base de la UI, el por defecto, o ya purgado)
bool MaterialPurge(Material* m);
// true = estaba en el cementerio y volvio a `Materials`
bool MaterialRevive(Material* m);
const std::vector<Material*>& MaterialsPurged();

// ===================================================
// Clase Material
// ===================================================
// capa de textura EXTRA (encima de la textura base del material): su textura, con QUE UV se dibuja y como se
// MEZCLA con lo de abajo. Render por MULTI-PASS (GL 1.1, portable PC+N95): cada capa es un draw mas sobre la
// misma superficie, con la textura PURA (sin luz ni color de vertice: eso ya lo aplico la base).
//   uvMap: indice de la capa UV de la malla con la que se dibuja (Mesh::uvMaps / Mesh::UVOfLayer). 0 = la de
//           siempre; 1 = la segunda capa (ej. el unwrap propio de un lightmap horneado, TEXCOORD_1 del glTF).
//   blend:  W3dLayerMix. "Multiply 2x" es el modo de los lightmaps con overbright (dst * src * 2).
enum W3dLayerMix {
    W3D_LAYER_MIX = 0,        // alpha encima (SRC_ALPHA, ONE_MINUS_SRC_ALPHA)
    W3D_LAYER_MULTIPLY = 1,   // DST_COLOR, ZERO (oscurece: sombras, lightmaps)
    W3D_LAYER_ADD = 2,        // ONE, ONE (aclara: brillos, luces)
    W3D_LAYER_MULTIPLY2X = 3, // DST_COLOR, SRC_COLOR = 2 * dst * src (lightmap con overbright hasta 2.0)
    W3D_LAYER_SUBTRACT = 4,   // resta (REVERSE_SUBTRACT en PC; aprox en GL ES 1.1)
    W3D_LAYER_SCREEN = 5,     // ONE, ONE_MINUS_SRC_COLOR (aclara suave)
    W3D_LAYER_MIXES = 6       // (cantidad; dejalo ultimo)
};
const char* W3dLayerMixName(int m);   // "Mix", "Multiply", ... (panel, harness, logs)

class TexLayer {
    public:
        Texture* tex;
        int blend;  // W3dLayerMix
        int uvMap;  // capa UV de la malla (0 = la base)
        bool on;
        float force;  // el FACTOR de la mezcla: 0 = no se ve, 1 = la capa entera (el % de la tarjeta)
        bool baked;   // LUZ HORNEADA (la tarjeta "Bake Lighting"): la textura ya tiene la luz. Mientras esta capa este
                        // prendida el material se dibuja SIN la luz de tiempo real (se sumaria dos veces); sacarla o
                        // apagarla devuelve la luz sola (Material::LightRealTime)
        bool maskSun;   // (horneada) el ALFA de la textura es CUANTO de la luz de cada texel es la luz directa del SOL
                           // (como el "Shadowmask" de Unity): la sombra en tiempo real de lo que se mueve le saca solo eso
                           // y deja la del ambiente y los rebotes, igual que la sombra horneada (se juntan sin costura).
                           // Sin la mascara (un horneado de antes) la sombra baja hasta el ambiente, sin pasarse
        TexLayer() : tex(NULL), blend(0), uvMap(0), on(true), force(1.0f), baked(false), maskSun(false) {}
};

// LOS MAPAS del material (la tarjeta Material de Properties, de arriba a abajo): el color base, el normal map y los del
// trazado de rayos / PBR (emision, rugosidad, metalico, alfa). Cada uno se lee con SU capa UV de la malla (uvMap) y los
// de valor (emision..alfa) pueden ser un NUMERO para toda la superficie o una TEXTURA. Claves del .w3d: "mapas".
enum W3dMatMap {
    W3D_MAP_BASE = 0,      // la textura del color (Material::texture)
    W3D_MAP_NORMAL,        // el normal map (Material::normalTexture)
    W3D_MAP_EMISSION,      // numero: el color de emision (emission[]); textura: su RGB
    W3D_MAP_ROUGHNESS,     // numero: rtRoughness; textura: su R (0 = espejo, 1 = mate)
    W3D_MAP_METALLIC,      // numero: rtMetallic; textura: su R
    W3D_MAP_ALPHA,         // numero: diffuse[3]; textura: su R (o su alfa si es RGBA)
    W3D_MAPS
};
const char* W3dMatMapKey(int map);            // "base", "normal", "emision", "rugosidad", "metalico", "alfa"
int         W3dMatMapByKey(const std::string& c);  // -1 = no es un mapa
class Material {
    public:
        // defaults en el constructor (Materials.cpp): los inicializadores
        // en la clase son C++11 y esto compila tambien en RVCT (C++03)
        bool textureOn;   // prender/apagar la textura (checkbox de la UI)
        bool filtered;    // filtrado de textura (lineal) o pixel perfect
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
        bool background;
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
        float bias_meters;
        int  order_pass; // EL PASE: 0 = opacos, 1 = calcomanias, 2 = transparentes. La escena se dibuja en
                           // tres pases (primero todo lo opaco, despues las calcomanias, al final lo transparente);
                           // adentro de cada pase manda el orden del arbol y el de las partes de malla (Mesh.cpp,
                           // g_w3dPassDrawing). El de calcomanias compara z con "menor o igual" (pegado a lo de abajo)
        int  mix;       // modo de mezcla cuando transparent = true. 0 = alpha (default, compatible
                           // con todo lo guardado hasta hoy); el resto son los codigos de gfx::Mezcla
                           // (2 = MixAdd = ADITIVO: GL_ONE/GL_ONE, aclara y el negro es invisible).
        float alphaTest;   // RECORTE POR ALFA (GL_ALPHA_TEST, fijo en GL ES 1.1): 0 = apagado; > 0 = se descartan los
                           // texels con alfa <= ref. Rejas, alambrados, hojas: SIN blend, con z y en la pasada opaca,
                           // asi no dependen del orden de dibujo (con blend el agujero escribia z y tapaba lo de atras).
        // TRAZADO DE RAYOS (editor, render/RayTracer): rugosidad 0..1 (0 = espejo, 1 = mate) y metalico 0..1
        // (0 = difuso; 1 = metal: refleja la escena teñido por su color). Metalico 1 + rugosidad 0 = espejo.
        float rtRoughness;
        float rtMetallic;
        bool chrome;      // "Reflection": reflejo de entorno (env-map). On/off; el MODO lo elige reflectMode.
        int  reflectMode; // 0 = Matcap (normal-del-ojo, matriz de textura, HARDWARE en PC y N95; rapido)
                          // 1 = Sphere Map exacto (GL_SPHERE_MAP: HARDWARE en PC via texgen, SOFTWARE en N95)
                          // 2 = Equirectangular 360 (SOFTWARE, calidad). (VGP exacto por HW = futuro, falta API de IMG)
        bool reflectionProbe; // el reflejo es el de LA SONDA DE REFLEJO MAS CERCANA (main/objects/ReflectionProbe.h): cada malla
                           // toma la imagen 360 de la sonda de su lugar (en vez de la textura). Clave: "reflectionProbe"
        bool normalMap;   // NORMAL MAPPING (DOT3): la textura 'normalTexture' perturba la normal por pixel.
                          // Multi-pass: base * (N.L). Excluyente con chrome (mismo combiner). Portable PC+N95.
        Texture* normalTexture; // el normal map (RGB = normal en tangent-space)
        // MEZCLA DE TEXTURAS en una pasada (gfx::MixTextures, multitextura): 'textureB' se interpola con la textura
        // base en el combiner de la unidad 1: fragmento = B * mixB + base (con su luz) * (1 - mixB). B sale plana
        // (sin luz). Claves del .w3d: "textureB" (ruta, como "normalTextura") y "mixB" (0..1; con 0 la unidad 1 ni
        // se prende). Lua: setTexturaB / setMezclaTextura / mezclaTextura; panel: "Texture B" / "Texture mix";
        // rutinas: el paso "Texture mix". No se combina con el reflejo ni con el normal map (van por el combiner)
        Texture* textureB;
        float    mixB;
        // SOMBREADO TOON (dibujo animado) sin shaders: la 'textureB' pasa a ser una RAMPA de una fila y la unidad 1
        // la lee con una coordenada calculada por vertice en la CPU, s = 0.5 + 0.5 * (normal . hacia la luz) (0 = de
        // espaldas a la luz, 1 = de frente), despues del skinning. 0 = apagado; 1 = la rampa se RESTA del color
        // (base - rampa: el color de la rampa es cuanto se oscurece); 2 = la rampa MULTIPLICA (base * rampa). Con
        // toon la luz de GL se apaga (la "luz" la pone la rampa). La luz es la primera direccional de la escena (o la
        // primera luz). Clave del .w3d: "toon" (con "textureB" = la rampa); glTF: extras w3d_toon (uri de la rampa)
        // + w3d_toonMix ("restar" / "multiplicar"). Corre igual en PC y en el N95 (GLES 1.1, dos unidades).
        int      toon;
        // CONTORNO del dibujo animado (casco invertido): > 0 = la malla se dibuja otra vez inflada sobre sus normales
        // posadas, en negro y mostrando solo las caras de atras, y asoma como un borde alrededor de la figura. El valor
        // es el grosor como fraccion de la distancia a la camara (ancho parejo en pantalla: 0.004 ~ 1 px en 240 lineas
        // con fov 55). Duplica los triangulos de la malla. Clave del .w3d: "contorno"; glTF: extras w3d_outline.
        float    outline;
        // SOMBREADO POR PIXEL (solo con shaders, gfx::ShadingPerPixelSupported): la luz se calcula en cada pixel en vez
        // de en cada vertice. Mas lindo (brillos redondos, luces cercanas) y mas caro. Clave del .w3d: "luzPorPixel"
        bool     perPixel;
        // el FACTOR de la capa de LUZ HORNEADA prendida (TexLayer::horneada), de 0 a 1 (0 = no hay)
        float FactorLightBaked() const {
            for (size_t c = 0; c < layers.size(); c++)
                if (layers[c].baked && layers[c].on && layers[c].tex)
                    return layers[c].force < 0.0f ? 0.0f : (layers[c].force > 1.0f ? 1.0f : layers[c].force);
            return 0.0f;
        }
        // la LUZ DE TIEMPO REAL: Lighting tildado y la luz horneada que no esta entera. Con la capa de luz horneada al
        // 100% solo esta la horneada; al 0% es como si no estuviera; en el medio, las dos mezcladas (Mesh.cpp)
        bool LightRealTime() const { return lighting && FactorLightBaked() < 0.999f; }
        bool     lightIndirect;   // LUZ INDIRECTA POR VERTICE en el pipeline fijo (el N95): con una sonda de luz horneada, la CPU
                                 // calcula la luz de cada vertice (las lamparas + la indirecta de la sonda con su normal).
                                 // Con shaders no hace falta: la luz por pixel ya la usa (main/objects/LightProbe.h)
        // ---- LINEAS (aristas de la malla dibujadas encima del relleno) ----
        // Con lineas = true, cada grupo que use este material dibuja ADEMAS sus
        // ARISTAS (los 3 lados de cada triangulo, deduplicados) con glLineWidth
        // = thicknessLine, en pixeles, respetando el color y la textura del
        // material. Si la malla es una TIRA degenerada (triangulos de area cero:
        // un camino de vertices), el relleno no pinta nada y las aristas dibujan
        // el CAMINO de la polilinea: una soga, un cable, un trazo.
        // .mtl: `lineas [grosor]` ; JSON del proyecto: "lineas" + "thicknessLine".
        bool  lines;         // default false
        float thicknessLine; // en px (default 1). El tope real lo pone el driver.
        int interpolation;
        Texture* texture;
        std::vector<TexLayer> layers; // capas de textura EXTRA encima de 'texture' (multi-pass)
        // ---- LOS MAPAS (W3dMatMap): BASE y NORMAL son 'texture' y 'normalTexture'; el resto, mapTex ----
        Texture* mapTex[W3D_MAPS];         // EMISION..ALFA (las de BASE y NORMAL quedan NULL aca)
        bool     mapWithTexture[W3D_MAPS]; // EMISION..ALFA: true = la textura; false = el NUMERO (default)
        int      uvMap[W3D_MAPS];          // la capa UV de la malla con la que se lee cada mapa (0 = la primera: "UV 1")
        float    ior;                      // indice de refraccion (trazado de rayos): 1 = nada, 1.33 = agua, 1.5 = vidrio
        // TRANSMISION (trazado de rayos) 0..1: cuanto de la superficie es VIDRIO (deja pasar la luz doblada por el IOR).
        // El alfa es otra cosa: lo transparente por el alfa (o por su mapa) no esta, y se ve lo de atras derecho. Con
        // alfa 50% y transmision 1: la mitad pasa derecho y la otra mitad refractada. 0 = el IOR no hace nada
        float    transmission;
        // REFLEJOS EN PANTALLA (luz por pixel con shaders): lo dibujado se refleja en el material (el piso mojado, el
        // metal pulido), segun la rugosidad, el metalico y el angulo (Fresnel). Una aproximacion sin profundidad
        bool     ssr;
        // SHADER ESCRITO (objects/WrittenShader.h): con shaderOn, en los backends con shaders el material se dibuja con
        // el archivo de texto 'shaderFile' ("shaders/vidrio.glsl" adentro del proyecto o una ruta del disco)
        bool        shaderOn;
        std::string shaderFile;
        int         shaderId;      // el programa compilado (0 = ninguno)
        int         shaderState;   // 0 = sin compilar, 1 = compilado, -1 = no compila (shaderError)
        std::string shaderError;
        bool        shaderScreen; // el shader lee la pantalla (w3d_Screen): se dibuja al final, sobre todo lo demas
        bool        shaderTime;   // usa w3d_time: se anima solo
        void*       shaderCPU;    // la seccion @vertex compilada para la CPU (objects/ShaderCPU.h): la usan los backends
                                    // SIN shaders (GLES 1.1, GL fijo). NULL = no hay @vertex o no hace falta
        Texture** MapSlot(int map);   // la ranura de la textura de ese mapa (NULL = no es un mapa)
        Texture*  MapTexture(int map) const;
        float diffuse[4];
        float specular[4];
        float emission[4];
        float ambient[4]; // reflectancia de la luz ambiente del material
        float shininess;  // brillo especular (exponente)
        // LA FUERZA DE LA EMISION: lo que emite es emission.rgb x emissionStrength (como el Strength de Blender). Tambien
        // multiplica el MAPA de emision en modo textura. Default 1 (lo de siempre). Clave del .w3d: "emissionStrength"
        float emissionStrength;
        // ---- LA TRANSFORMACION UV (formato/material-animado.md): la UV con la que se leen la textura base y los mapas
        //      (no las capas extra ni la textura B) pasa por
        //          uv' = R(uvRotation) * S(uvScale) * (uv - uvPivot) + uvPivot + uvOffset
        //      R gira las COORDENADAS (U hacia V; la imagen gira para el otro lado); el desplazamiento se SUMA a la UV
        //      (+U corre la imagen hacia -U, como el Location del Mapping de Blender en modo Point). Rotacion en grados.
        //      Defaults: 0,0 / 1,1 / 0 / 0.5,0.5. Lo aplica cada backend con la matriz de textura (gfx::TexMatrixUVAffine)
        //      y el trazado de rayos en su UV del impacto. Claves: "uvOffset", "uvScale", "uvRotation", "uvPivot" ----
        float uvOffset[2];
        float uvScale[2];
        float uvRotation;
        float uvPivot[2];
        // true = la transformacion UV no es la identidad (sin ella no se arma ni se carga ninguna matriz)
        bool UVTransformed() const {
            return uvOffset[0] != 0.0f || uvOffset[1] != 0.0f || uvScale[0] != 1.0f || uvScale[1] != 1.0f || uvRotation != 0.0f;
        }
        // la matriz AFIN de la transformacion UV, por columnas: u' = m[0]*u + m[2]*v + m[4] ; v' = m[1]*u + m[3]*v + m[5]
        void UVMatrix(float m[6]) const;
        // IDENTIDAD ESTABLE (como Object::serial): un numero que no se recicla ni al renombrar ni al borrar. Lo usa el dope
        // sheet para nombrar la pista de un material en sus claves de seleccion (el nombre se libera al renombrar)
        unsigned serial;
        // EL CABEZAL LO POSO alguna vez (una pista de material le escribio un canal): el guardado lo devuelve a su
        // reposo (W3dMaterialsRestBegin) solo si es asi, como Object::posedByCurves
        bool posedByCurves;
        std::string name;
        // CARPETA cosmetica del outliner por recursos ("Piel/Enemigos"; "" = la raiz). No
        // afecta al render ni al runtime: solo ordena la vista de materiales del editor.
        std::string folder;
        // LA LIBRERIA externa de la que es ("" = del proyecto). Un material de libreria vive en SU espacio de
        // nombres (el nombre lleva el prefijo: "Personajes/Piel"), es de SOLO LECTURA y no se guarda en el
        // proyecto (el que lo nombra guarda solo su nombre). Ver main/io/Libraries.h.
        std::string lib;

        Material(const std::string& newName, bool MaterialDefault = false, bool HasVertexColor = false);
        ~Material();

        // LA PUERTA del rename: uniquifica en el espacio GLOBAL de materiales.
        void SetName(const std::string& newName);
};

// ===================================================
// Funciones auxiliares
// ===================================================
Material* FindMaterialByName(const std::string& name);
// nombre libre en el espacio GLOBAL de materiales ('excepto' conserva el suyo).
std::string MaterialFreeName(const std::string& base, const Material* except);

// ===================================================
//  LA RECETA DE LA CALCOMANIA, EN UN SOLO LUGAR
//
//  "decal" no es un flag: son CUATRO campos que van juntos (transparente + no
//  escribir z + polygon offset hacia el ojo + orden de pasada 1). La palabra
//  `decal` del .mtl, el tilde "Decal" del panel de materiales y el assert
//  `matflags <mat> decal` del harness describian LO MISMO en tres lugares
//  distintos: ahora los tres llaman aca.
//    W3dMaterialIsDecal      -> el material cumple la receta entera
//    W3dMaterialApplyDecal -> la aplica (on) o la deshace (off). 'unidades' es el
//                               polygon offset; 0 usa el default (-4, que anda en PC
//                               con z de 24 bits; en el N95 el render lo multiplica x4).
// ===================================================
bool W3dMaterialIsDecal(const Material* m);
void W3dMaterialApplyDecal(Material* m, bool on, float units = 0.0f);

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
bool W3dMaterialIsLight(const Material* m);

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
bool HasAnimationActive();

#endif