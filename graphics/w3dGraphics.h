#pragma once
// ============================================================================
//  Whisk3DCore (engine) — abstraccion del ESTADO de graficos
//
//  NUNCA uses llamadas directas. para eso esta Whisk3D core
// NO llaman a OpenGL directo: usan estas funciones. El
//  motor las resuelve segun el backend elegido en compile-time (hoy: pipeline
//  fijo de GL de escritorio y GL ES 1.1, que comparten esta misma API de
//  estado; manana: GL ES 2.0, Vulkan, DirectX, con su propia implementacion).
//
//  Esta cabecera NO expone enums de OpenGL: usa enums propios para que quien la
//  use no dependa de ningun header de graficos. Ver ARQUITECTURA.md.
//
// mas adelante tengo que ver como permitir usar mas de un backend grafico en los sistemas que lo soporten. como PC
// ============================================================================

#include "crossplatform.h" // MeshIndex (16 bits N95 / 32 bits escritorio)

#include "w3dBackend.h"   // (W3D_BACKEND_OPEN / CLOSE: varios backends en un ejecutable)
namespace w3dEngine {

    // --- capacidades que se prenden / apagan ---
    enum Cap {
        DepthTest,      // test de profundidad
        CullFace,       // descarte de caras traseras
        Texture2D,      // texturizado 2D
        Blend,          // mezcla alfa
        Lighting,       // iluminacion del pipeline fijo
        Normalize,      // renormalizar normales tras escalar
        Fog,            // niebla
        ColorMaterial,  // color por vertice -> material
        ScissorTest,    // recorte por rectangulo
        PolygonOffsetFill, // offset de profundidad del relleno (z-fighting)
        PointSprite,    // sprites de punto (coord de textura por punto)
        Dither,         // dithering
        Light0,         // luz 0 del pipeline fijo
        Multisample,    // anti-aliasing MSAA (hay que APAGARLO en el color-ID pick)
        CapCount_       // (cantidad; dejalo ultimo, los backends dimensionan arrays con esto)
    };
W3D_BACKEND_OPEN
    void Enable(Cap c);
    void Disable(Cap c);
W3D_BACKEND_CLOSE

    // comparacion del depth test
    //  DepthEqual : pasadas extra sobre la MISMA superficie (normal map, capas)
    //  DepthAlways: ESCRIBIR profundidad a la fuerza. Lo usa el espejo para
    //               "borrar" la z adentro de la mascara de estencil. OJO: NO se
    //               puede hacer con Clear(DepthBuffer), porque el glClear NO pasa
    //               por el test de estencil (solo por el scissor y los writemasks,
    //               spec GL 2.1 §4.2.3): un Clear ahi borraria la profundidad de
    //               TODA la ventana y el agua se dibujaria encima de todo.
    enum DepthCmp { DepthLess, DepthLEqual, DepthEqual, DepthAlways };
W3D_BACKEND_OPEN
    void DepthFunc(DepthCmp c);
W3D_BACKEND_CLOSE

    // --- arrays de vertices del pipeline fijo ---
    enum VArray { VertexArray, TexCoordArray, NormalArray, ColorArray };
W3D_BACKEND_OPEN
    void EnableArray(VArray a);
    void DisableArray(VArray a);
    int  ArrayState(VArray a);   // lo que sabe el cache: 1 prendido, 0 apagado, -1 no se sabe (el editor valida con esto)
W3D_BACKEND_CLOSE

    // --- buffers ---
W3D_BACKEND_OPEN
    void ClearColor(float r, float g, float b, float a);
W3D_BACKEND_CLOSE
    enum { ColorBuffer = 1, DepthBuffer = 2, StencilBuffer = 4 };
W3D_BACKEND_OPEN
    void Clear(int bits); // ColorBuffer | DepthBuffer | StencilBuffer
W3D_BACKEND_CLOSE

    // --- PLANOS DE RECORTE (user clip planes) ---
    // Cortan la geometria por planos ARBITRARIOS: se dibuja solo el semiespacio
    // a*x + b*y + c*z + d >= 0. Es lo que el scissor NO puede hacer (el scissor
    // recorta un rectangulo de PANTALLA; esto recorta en el MUNDO).
    // Lo usa el reflejo del espejo para no salirse del cuerpo de agua ni asomar
    // por encima de la superficie.
    // *** Las ecuaciones van en el espacio de la matriz MODELVIEW ACTIVA en el
    //     momento de la llamada (igual que glClipPlane). Quien tenga el plano en
    //     MUNDO y la matriz de mundo M del objeto que esta dibujando lo pasa
    //     como M^T * plano. ***
    // n = 0 los apaga todos. Pipeline fijo: GL_CLIP_PLANEi. GLES2/WebGL: discard
    // en el shader (los primeros PlanesClipMax()).
W3D_BACKEND_OPEN
    int  PlanesClipMax();                        // cuantos soporta el backend/driver
    void PlanesClip(int n, const float* eq4);    // n planos, 4 floats cada uno
W3D_BACKEND_CLOSE

    // --- ESTENCIL: recorte por FORMA ---
    // La unica forma de acotar una pasada a algo que NO es un rectangulo de
    // pantalla ni un semiespacio: se MARCA una silueta (dibujandola con z-test,
    // asi lo que este DELANTE no la marca) y despues se dibuja solo adentro de
    // la marca. El espejo del agua lo usa para que el reflejo quede recortado a
    // la lamina de agua REALMENTE VISIBLE (si un puente tapa el estanque, el
    // reflejo no se pinta encima del puente).
    // StencilBits() = 0 cuando el contexto se creo sin buffer de estencil: el
    // que llama tiene que poder seguir sin el (el espejo cae al recorte por planos).
W3D_BACKEND_OPEN
    int  StencilBits();
    void StencilClear(int value);    // pone TODO el buffer en 'valor'
    void StencilMark(int value);     // test siempre pasa y ESCRIBE 'valor' donde se dibuje
    void StencilInside(int value);   // dibuja SOLO donde el buffer vale 'valor' (no escribe)
    void StencilTurnOff();           // desactiva el test (y deja el buffer sin escribir)
W3D_BACKEND_CLOSE

    // --- viewport y recorte (coordenadas ya en el sistema del backend) ---
W3D_BACKEND_OPEN
    void Viewport(int x, int y, int w, int h);
    void Scissor(int x, int y, int w, int h);
W3D_BACKEND_CLOSE
    // lectura del estado actual (para guardar y reponer: un objeto que dibuja en un sub-rectangulo)
W3D_BACKEND_OPEN
    void GetViewport(int xywh[4]);
    void GetScissor(int xywh[4]);
    bool ScissorActive();
W3D_BACKEND_CLOSE

    // --- matrices del pipeline fijo ---
    enum Matrix { Projection, ModelView, TextureMatrix };
W3D_BACKEND_OPEN
    void MatrixMode(Matrix m);
    void LoadIdentity();
W3D_BACKEND_CLOSE

    // pila de matrices + traslacion (la UI 2D posiciona asi). El backend GL las hace con
    // glPushMatrix/glTranslatef; un backend Vulkan/DX las haria con Matrix4 explicita.
W3D_BACKEND_OPEN
    void PushMatrix();
    void PopMatrix();
    void Translatef(float x, float y, float z);
    void Rotatef(float angleDeg, float x, float y, float z); // rotacion sobre un eje
    void Scalef(float x, float y, float z);
    void MultMatrix(const float* m16);  // multiplica la matriz actual por m (column-major)
    void GetMatrix(Matrix m, float* out16); // la matriz ACTUAL de ese stack: de la SOMBRA en la CPU (sin glGet, que en el N95 espera al GPU)
W3D_BACKEND_CLOSE
    extern bool g_invalidatePerMesh;        // Invalidate() en cada malla (el editor de Symbian; ver Mesh::RenderObject)
    extern bool g_matrixAudit;              // (harness 'matrizaudit') cada GetMatrix compara la sombra con GL...
    extern int  g_matrixDesyncs;            // ...y cuenta las que no dan igual
W3D_BACKEND_OPEN
    void LoadMatrix(const float* m16);   // carga m como matriz actual (ej: la vista de CameraBase)
W3D_BACKEND_CLOSE
    // PILA PEREZOSA de la MODELVIEW (pipeline fijo): Push/Pop/Mult/Load/LoadIdentity/Translatef/Rotatef/Scalef solo
    // cambian la SOMBRA en la CPU; la matriz llega a GL (UN glLoadMatrixf, si cambio) recien antes de lo que la usa:
    // los Draw*, la posicion de las luces (Light0fv/LightNfv) y los planos de recorte. Un objeto que no dibuja (su malla
    // quedo afuera del autocull) no cuesta ninguna llamada; uno que dibuja, 1 en vez de 3. Sin la matriz en la sombra
    // (una pila mas honda que la sombra) vuelve a lo inmediato. GL CRUDO que use la modelview (glLightfv de la posicion,
    // un glDraw*, un glGet de la matriz): MatricesSync() antes. GLES2/WebGL ya era perezoso (su pila vive en la
    // CPU y va por uniforms en cada draw): ahi Sincronizar no hace nada
    extern bool g_matricesLazy;     // default true; false = cada operacion va a GL como antes (A/B: harness 'perezosas')
W3D_BACKEND_OPEN
    void MatricesSync();
W3D_BACKEND_CLOSE
    // el SESGO EN METROS: la proyeccion del pase con la profundidad corrida m metros de camara (+ aleja, - acerca) sin
    // mover la geometria en pantalla. Estado como los demas: solo llama al driver si cambia (0 = la del pase)
W3D_BACKEND_OPEN
    void BiasMeters(float m);
W3D_BACKEND_CLOSE

    // proyecciones (cargan/multiplican la matriz PROJECTION activa)
W3D_BACKEND_OPEN
    void Ortho(float l, float r, float b, float t, float n, float f);
    void Frustum(float l, float r, float b, float t, float n, float f);
    void Perspective(float fovYDeg, float aspect, float n, float f); // via Frustum (sin GLU)
W3D_BACKEND_CLOSE

    // --- estado suelto ---
W3D_BACKEND_OPEN
    void DepthMask(bool write);     // permitir/escribir el z-buffer
    void LineWidth(float px);
    void SmoothShading(bool smooth);  // GL_SMOOTH (true) / GL_FLAT (false)
    void FastPerspective();          // hint de correccion de perspectiva rapida
    void FrontFace(bool ccw);        // winding de la cara frontal (true=CCW)
    void CullFrontalFaces(bool on);  // descartar las caras FRONTALES (glCullFace(GL_FRONT)) en vez de las traseras: el
W3D_BACKEND_CLOSE
                                       // contorno de casco invertido (Material::contorno). Vale con cualquier winding
W3D_BACKEND_OPEN
    void ColorMask(bool r, bool g, bool b, bool a); // permitir/bloquear canales de color
    void PointSpriteCoordReplace(bool on); // genera coords de textura por punto (point sprites)
W3D_BACKEND_CLOSE

    // --- textura activa (con CACHE: ver abajo) ---
    // Bindea la textura 2D. El motor recuerda cual esta puesta: si ya es esa,
    // NO hace la llamada GL (ahorra el bind, que es caro). id 0 = ninguna.
W3D_BACKEND_OPEN
    void BindTexture(unsigned int id);
W3D_BACKEND_CLOSE

    // CHROME (env-map esferico / matcap): genera las UV desde el normal -> reflejo falso. PC = GL_SPHERE_MAP
    // del pipeline fijo (hardware). Symbian/GLES1 NO tiene glTexGen: queda como stub (UV por software TODO).
W3D_BACKEND_OPEN
    void TexGenSphere(bool on);
W3D_BACKEND_CLOSE

    // MATCAP por HARDWARE sin texgen (anda en GLES1/N95): arma la matriz de TEXTURA = rotacion del modelview x
    // escala/bias (+ flip-V), y las NORMALES se mandan como texcoords (TexCoordPointer3b) -> la GPU calcula la UV
    // del matcap por vertice. on=arma la matriz; off=la resetea a identidad. (Es el "normal del ojo", aprox del
    // GL_SPHERE_MAP exacto.) NO mezclar con TexGenSphere (los dos tocan la matriz de textura).
W3D_BACKEND_OPEN
    void TexMatrixMatcap(bool on);
W3D_BACKEND_CLOSE
    // la matriz de textura como DESPLAZAMIENTO y ESCALA de las UV (u' = u*su + du): mover un reflejo o una cinta sin
    // reescribir las UV. (0,0,1,1) = identidad. Comparte el cache del matcap: TexMatrixMatcap(false) la resetea
W3D_BACKEND_OPEN
    void TexMatrixUV(float du, float dv, float su, float sv);
W3D_BACKEND_CLOSE
    // la matriz de textura como una transformacion AFIN de las UV, por columnas: u' = m[0]*u + m[2]*v + m[4],
    // v' = m[1]*u + m[3]*v + m[5]. Es la TRANSFORMACION UV de un material (desplazamiento + escala + ROTACION:
    // Material::UVMatrix, formato/material-animado.md). NULL = la identidad. Comparte el cache del matcap y de
    // TexMatrixUV: TexMatrixMatcap(false) la resetea
W3D_BACKEND_OPEN
    void TexMatrixUVAffine(const float* m6);
W3D_BACKEND_CLOSE

    // true si hay glTexGen (sphere-map EXACTO por hardware: PC). false en GLES1 (N95) -> ese modo cae a software.
W3D_BACKEND_OPEN
    bool HasTexGen();
W3D_BACKEND_CLOSE

    // chrome = espejo: textura DIRECTA (GL_REPLACE), independiente de la luz. false = GL_MODULATE normal.
    // Lo usan los dos modos de chrome (matcap y equirect). Anda en PC y GLES1 (glTexEnvi existe en ambos).
W3D_BACKEND_OPEN
    void TexEnvReplace(bool on);
    void TexEnvDot3(bool on); // DOT3 normal mapping: combiner N.L (textura=N, color=L). Reset=MODULATE
W3D_BACKEND_CLOSE
    // SOMBREADO POR PIXEL: la iluminacion se calcula en cada pixel (fragment shader) en vez de en cada vertice y
    // interpolarse. Solo los backends con shaders (GLES 2.0 / WebGL); en el pipeline fijo (GLES 1.1, la PC fija, la 3DS)
    // no existe: Soportado() da false y el editor ni ofrece la opcion
W3D_BACKEND_OPEN
    void ShadingPerPixel(bool on);
    bool ShadingPerPixelSupported();
    void TexEnvAlphaOnly(bool on); // pases planos: RGB=color de vertice, ALPHA=alpha de la textura. Reset=MODULATE
W3D_BACKEND_CLOSE

    // PASES CON SHADER (GLES 2.0 / WebGL / Vulkan). Lo que en el pipeline fijo se pinta POR VERTICE desde la CPU (la normal
    // hecha color, el movimiento en la pantalla) lo calcula la GPU en cada pixel, y la vista Solido usa un programa chico.
    // Con PassesWithShader() en false (GL 1.1, GLES 1.1, la 3DS) el editor sigue con el camino de la CPU. Mientras un pase
    // esta prendido, los TRIANGULOS lo usan (las lineas y los puntos no). Los parametros de PassVectors / PassData:
    // [0..15] la matriz PROYECCION x VISTA del cuadro anterior, [16] [17] el ancho y el alto de la imagen (px), [18] el
    // lejos (m: la escala de la profundidad de PassData), [19..34] la PROYECCION x VISTA de la imagen ENTERA de ahora (en
    // un render por tiles la de GL es la del tile). Las posiciones del anterior van en PrevPointer3f (las
    // mismas del draw, en el mismo orden; NULL = quieto)
    enum PassShader { PassNone = 0, PassNormal = 1, PassSolid = 2, PassVectors = 3, PassData = 4 };
    // PassData = la PROFUNDIDAD (distancia / lejos) en 24 bits: R alta, G media, B baja (sin alfa: la pantalla puede no
    // tenerlo). POSTPROCESADO con shader (PostEffect): un quad sobre el viewport con el efecto, leyendo la imagen y sus
    // datos (la profundidad de PassData o el movimiento de PassVectors) de texturas copiadas de la pantalla. uv4 = el
    // rectangulo de la imagen en la textura (u0, v0 abajo a la izquierda; u1, v1 arriba a la derecha)
    // (el BLOOM son dos: el desenfoque de un eje -que en la primera pasada filtra el brillo- y la mezcla con la imagen)
    enum PostShader { PostCopy = 0, PostGrain = 1, PostFocus = 2, PostMotion = 3, PostBloomBlur = 4, PostBloomMix = 5 };
W3D_BACKEND_OPEN
    bool PassesWithShader();
    void SpecialPass(int mode, const float* p);
    void PrevPointer3f(const float* p);
    bool PostEffect(int effect, unsigned int texImg, unsigned int texDat, int texW, int texH, const float* uv4, const float* p8);
W3D_BACKEND_CLOSE

    // SHADERS ESCRITOS (el "Shader personalizado" de un material; docs/manual/<idioma>/editor/shaders.md). El usuario
    // escribe solo SUS funciones (Fragmento y, si quiere, Vertice) con los nombres de Whisk3D (w3d_uv, w3d_normal,
    // w3d_Screen...): el BACKEND les pone adelante lo suyo (el prologo) y el main. Asi el mismo archivo anda en todos los
    // backends con shaders (GLES 2.0 de Android/web/N8 y Vulkan); los otros (GL fijo, GLES 1.1, la 3DS) dicen false en
    // ShadersWritten y el material se dibuja como siempre.
    //   WrittenShaderCompile: > 0 = el programa; 0 = no compilo (el error en log, con los renglones del archivo)
    //   WrittenShaderUse: los TRIANGULOS de los draws que siguen lo usan (0 = apagado). p[ShaderWrittenParams]:
    //     [0] tiempo (s), [1] ior, [2] transmision, [3] rugosidad, [4] metalico, [8..16] la rotacion de la VISTA al
    //     MUNDO (mat3 por columnas: w3d_viewToWorld, para leer un mapa de entorno). Si el shader lee la pantalla
    //     (w3d_Screen), cada draw la copia antes
    // Las lineas y los puntos no lo usan; un pase especial (SpecialPass) le gana.
    enum { ShaderWrittenParams = 20 };
W3D_BACKEND_OPEN
    bool ShadersWritten();
    int  WrittenShaderCompile(const char* vertex, const char* fragment, char* log, int logMax);
    void WrittenShaderDelete(int id);
    void WrittenShaderUse(int id, const float* p);
W3D_BACKEND_CLOSE

    // MEZCLA DE TEXTURAS en UNA pasada (multitextura): la unidad 1 interpola SU textura (B) con lo que salio de la
    // unidad 0 (la textura del material con su luz / color / modo): fragmento = B * k + unidad0 * (1 - k). k (0..1)
    // es el alfa del color constante del combiner (GL_INTERPOLATE): cambiarlo por cuadro es un glTexEnv, sin tocar
    // la geometria ni dibujar dos veces. B sale PLANA (la luz entra por la unidad 0). La unidad 1 usa las MISMAS UV
    // que la 0 (los punteros y el array de UV se replican mientras esta prendida) y toma el filtro / la repeticion
    // que se le pasan. GL 1.3 / GL ES 1.1 (2 unidades garantizadas: el N95 las tiene); sin soporte no hace nada.
    // Off = vuelve a una unidad (lo llaman ApplyMaterial, el fin del dibujo de la malla y BaseState).
    // 'modo': 0 = INTERPOLATE (B * k + unidad0 * (1 - k)); 1 = MULTIPLY (unidad0 * B: un lightmap en la misma
    // pasada, k no se usa); 2 = MULTIPLY 2x (unidad0 * B * 2: el lightmap con overbright, GL_RGB_SCALE); 3 = SUBTRACT
    // (unidad0 - B: la rampa del sombreado toon, Material::toon); 4 = MULTIPLY SIMPLE (unidad0 * B con el GL_MODULATE
    // clasico, sin el combiner: el toon "multiplicar", que anda en cualquier driver GL ES 1.1); 5 = SUMA (unidad0 + B
    // con el GL_ADD clasico: lo autoiluminado de un material, la mascara ya multiplicada en B).
W3D_BACKEND_OPEN
    bool MixTexturesSupported();
    void MixTextures(unsigned int idB, float k, bool filtered, bool repeat, int mode = 0);
    void MixTexturesOff();
    bool MixTexturesActive();
W3D_BACKEND_CLOSE
    // la unidad 1 con SUS PROPIAS UV (el unwrap de un lightmap) en vez de replicar las de la unidad 0: un puntero de RAM
    // (2 floats por vertice) o un VBO. NULL / 0 = vuelve a las mismas UV que la unidad 0. Off las olvida.
W3D_BACKEND_OPEN
    void MixTexturesUV(const float* p);
    void MixTexturesUV2s(const short* p);   // (2 short por vertice: la escala, en la TextureMatrix de la unidad 1)
    void MixTexturesUVVBO(unsigned int h);
W3D_BACKEND_CLOSE
    // la matriz de textura de la UNIDAD 1 como desplazamiento y escala de sus UV (u' = u*su + du), como TexMatrixUV con
    // la 0: las dos capas de un material animado corren cada una a su velocidad. (0,0,1,1) = identidad (cacheada: pedir
    // la identidad cuando ya esta no cuesta nada). Queda puesta hasta que se cambie: quien la mueve la devuelve
W3D_BACKEND_OPEN
    void MixTexturesMatrixUV(float du, float dv, float su, float sv);
W3D_BACKEND_CLOSE
    // (DeleteTexture la llama: GL desbindea lo borrado de todas las unidades y el cache de la 1 lo tiene que saber)
W3D_BACKEND_OPEN
    void MixDeletedTexture(unsigned int id);
W3D_BACKEND_CLOSE
    // el ultimo error de GL (glGetError; 0 = ninguno). Lo usa quien sube buffers grandes (WorldBsp) para saber si el
    // driver los acepto: en el N95 un glBufferData que no entra falla EN SILENCIO y el VBO queda vacio
W3D_BACKEND_OPEN
    unsigned int ErrorGL();
W3D_BACKEND_CLOSE

    // --- color uniforme del pipeline fijo (cuando NO hay array de color) ---
W3D_BACKEND_OPEN
    void Color4f(float r, float g, float b, float a);
    void Color4fv(const float* rgba);
    void Color4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
W3D_BACKEND_CLOSE

    // --- MATERIAL del pipeline fijo ---
    // El motor resuelve float (GL escritorio / GL ES 1.1 de Symbian) vs punto
    // fijo (GL ES 1.0 de Android viejo, glMaterialx*) adentro: quien lo use
    // pasa siempre float y NO necesita #ifdef ANDROID. Cara = FRONT_AND_BACK.
    enum MatParam { MatAmbient, MatDiffuse, MatSpecular, MatEmission };
W3D_BACKEND_OPEN
    void Material(MatParam p, const float* rgba); // 4 floats
    void MaterialShininess(float s);
    // LOS VALORES PBR del material para la luz POR PIXEL de los shaders (GLES2 / WebGL / Vulkan): metalico, rugosidad,
    // F0 (del IOR: cuanto refleja de frente) y transmision, como los usa el trazado de rayos. rug < 0 = apagado (la luz
    // de siempre, la del pipeline fijo con Material/MaterialShininess). En los backends fijos no hace nada
    void MaterialPBR(float metal, float rug, float f0, float trans, bool ssr = false);   // ssr: reflejos en pantalla
    // LOS MAPAS DEL MATERIAL con textura (ids de GL; 0 = el numero): normal, rugosidad, metalico, alfa y emision. true = este
    // backend los aplica en el shader (si no, el que llama usa lo suyo: el normal map por la pasada DOT3)
    bool MaterialMaps(unsigned normal, unsigned roughness, unsigned metallic, unsigned alpha, unsigned emission);
    // LA LUZ INDIRECTA de una SONDA DE LUZ para el objeto que se dibuja: 9 coeficientes RGB (27 floats, armonicos
    // esfericos en el espacio del MUNDO, ya con la convolucion del coseno) y la matriz 3x3 (por columnas) que pasa una
    // direccion del OJO al mundo. La luz por pixel PBR la usa de ambiente. NULL = apagada. Los backends fijos: nada
    void AmbientSH(const float* sh27, const float* eyeToWorld9);
W3D_BACKEND_CLOSE
    // LAS SOMBRAS EN TIEMPO REAL (main/render/RealTimeShadows.h). EL MAPA DE SOMBRAS es la profundidad de los objetos vista
    // desde la luz, en una textura del backend: ShadowMapBegin la prepara (w x h texeles, todo lejos) y desde ahi cada draw
    // de triangulos escribe su profundidad (sin color ni luz) hasta ShadowMapFinish, que vuelve a la pantalla como estaba
    // (viewport y scissor). ShadowMapBox: el rectangulo del mapa de cada CASCADA (como un viewport). Despues,
    // ShadowReceptors (ShadowDataN floats; NULL = sin sombras en este pase) y ShadowReceive(modo) por material: 0 no las
    // recibe; 1 con la luz de tiempo real (la luz que da sombra no llega donde algo la tapa); 2 sobre la luz HORNEADA (el
    // color se oscurece solo donde lo tapa algo que SE MUEVE: la sombra de lo estatico ya esta en la textura horneada, como
    // el "Shadowmask" de Unity). El mapa guarda DOS profundidades: la MASCARA DE COLOR elige cual se escribe (RG: todo lo que
    // proyecta; BA: solo lo que se mueve, en una pasada aparte con el z-buffer limpio). false / nada en los backends sin shaders (el GL fijo y GL ES 1.1, la PSP, la
    // 3DS): ahi las sombras las arma RealTimeShadows.cpp con la CPU. Los datos:
    //   [0..63]  4 matrices (por columnas) OJO -> (u, v, profundidad) del mapa, una por cascada
    //   [64..79] el rectangulo de cada cascada en el mapa (u0, v0, u1, v1)
    //   [80..83] hasta donde llega cada cascada (distancia a la camara, en el eje de la vista)
    //   [84..87] el desenfoque de cada cascada (texeles; 0 = el filtro de 2x2)
    //   [88..91] el corrimiento por la normal de cada cascada (unidades del ojo: contra el "acne")
    //   [92..95] ancho y alto del mapa, donde empieza a desvanecerse y la distancia maxima
    //   [96..99] intensidad (0..1), cantidad de cascadas, sesgo de la profundidad, suaves (1) o duras (0)
    //   [100]    la luz que da sombra (su numero de GL: 0..7)
    enum { ShadowDataN = 104 };
W3D_BACKEND_OPEN
    bool ShadowsSupported();
    bool ShadowMapBegin(int w, int h);
    void ShadowMapBox(int x, int y, int w, int h);
    void ShadowMapFinish();
    void ShadowReceptors(const float* data);
    void ShadowReceive(int mode);
    // DIBUJAR FUERA DE LA PANTALLA (la sonda de reflejo de tiempo real: sus 6 caras): CaptureBegin arma una imagen de
    // w x h (RGBA + profundidad) y desde ahi todo se dibuja ahi -el viewport es toda la imagen, sin recorte- hasta
    // CaptureFinish, que vuelve a la pantalla como estaba (viewport, recorte y color de borrado). ReadPixelsRGBA, en el
    // medio, lee la imagen. false: este backend no puede (los sin shaders) o la GPU es chica (un programa por draw)
    bool CaptureBegin(int w, int h);
    void CaptureFinish();
    // EL ENTORNO (el fondo como textura 360) para la luz PBR por pixel: 'tex' la imagen, 'blurryTex' una copia chica y
    // borrosa (la luz difusa y los reflejos rugosos; 0 = la misma) y 'eyeToImage' la matriz 3x3 (filas) que pasa una
    // direccion del OJO a la de la imagen (la vista invertida y la rotacion del fondo). tex = 0: sin entorno. Los
    // backends fijos no hacen nada (el cielo se ve igual: es una esfera texturada)
    void Environment(unsigned int tex, unsigned int blurryTex, const float* eyeToImage);
    // LO DE ATRAS para el VIDRIO de la luz por pixel (transmision): on = copia lo que ya esta dibujado en el viewport a
    // una textura que el shader lee refractado por el IOR y desenfocado por la rugosidad (antes del vidrio se dibujo
    // todo lo opaco); off = deja de usarla. Los backends fijos no hacen nada (ahi el vidrio es mezcla)
    void SceneCopy(bool on);
    // una GPU CHICA (la VideoCore IV de Android 2.3: un programa por draw): el editor dibuja con menos detalle lo que no
    // se nota (el cielo con menos triangulos, el 360 de los reflejos desde una copia chica)
    bool GpuLimited();
    // EL ENTORNO COMO CUBEMAP (las GPU chicas): las 6 caras (RGBA, lado x lado, en el orden de GL: +X -X +Y -Y +Z -Z) y
    // el entorno con dos cubos (el nitido y el borroso) en vez de las dos imagenes 360. El shader lee el cubo con la
    // direccion: sin las cuentas de la imagen 360 (atan y acos en cada pixel). UploadCube da 0 donde no hay cubemaps
    unsigned int UploadCube(const unsigned char* const faces[6], int side);
    void EnvironmentCube(unsigned int cube, unsigned int blurryCube, const float* eyeToImage);
W3D_BACKEND_CLOSE

    // --- niebla (fog) del pipeline fijo ---
W3D_BACKEND_OPEN
    void FogStart(float z);
    void FogEnd(float z);
    void FogColor(const float* rgba);
W3D_BACKEND_CLOSE
    // modo completo (el objeto Niebla): 0 = LINEAL (start/end), 1 = EXP, 2 = EXP2 (densidad). GL fijo lo
    // calcula por vertice (barato en el N95); el backend GLES2 lo replica en el shader.
W3D_BACKEND_OPEN
    void FogMode(int mode);
    void FogDensity(float d);
W3D_BACKEND_CLOSE

    // --- luz 0 del pipeline fijo ---
    // LightArea (solo LightNfv, solo los backends con shaders): la LUZ DE AREA (Light.h). v = medio ancho y medio alto en
    // su espacio local, 1 = elipse; el backend los lleva a OJO con la modelview ACTIVA (como la posicion) y alumbra hacia
    // la direccion del cono (LightNSpot). La posicion o el cono la vuelven a una luz comun: va DESPUES de los dos. Los
    // backends sin shaders la ignoran (Light.cpp ya la armo como foco)
    enum LightFv { LightAmbient, LightDiffuse, LightSpecular, LightPosition, LightArea };
W3D_BACKEND_OPEN
    void Light0fv(LightFv p, const float* v);
W3D_BACKEND_CLOSE
    enum LightF { LightConstantAtt, LightLinearAtt, LightQuadraticAtt };
W3D_BACKEND_OPEN
    void Light0f(LightF p, float v);
    void SetLightEnabled(unsigned int glLightId, bool on); // habilita/apaga una luz por su id GL
W3D_BACKEND_CLOSE
    // --- luces por NUMERO (0..LightsMax-1 = GL_LIGHT0..7): las usa la Rutina, que fija cada estado suelto. La
    //     posicion se transforma con la modelview ACTIVA (como glLightfv): w = 0 direccional, 1 puntual. Fuera de
    //     rango = nada. GLES2/WebGL las emula en el shader con la misma cuenta del pipeline fijo (por vertice). ---
    enum { LightsMax = 8 };
W3D_BACKEND_OPEN
    void LightNfv(int i, LightFv p, const float* v);
    void LightNf(int i, LightF p, float v);        // atenuacion de la luz i (glLightf)
W3D_BACKEND_CLOSE
    // cono de la luz i: corte en grados (180 = sin cono), exponente de concentracion y direccion (en la modelview
    // ACTIVA, como glLightfv(GL_SPOT_DIRECTION))
W3D_BACKEND_OPEN
    void LightNSpot(int i, float cut, float exponent, const float* dir3);
    void LightN(int i, bool on);                   // glEnable/glDisable(GL_LIGHTi) (la 0 pasa por el cache: Light0)
    void LightModelAmbient(const float* rgba);     // glLightModelfv(GL_LIGHT_MODEL_AMBIENT)
    void LightModelTwoSide(bool on);               // glLightModelf(GL_LIGHT_MODEL_TWO_SIDE): las caras de atras con la normal dada vuelta
W3D_BACKEND_CLOSE

    // query: usa el CACHE del motor (NO llama glIsEnabled, que flushea el GPU en el N95)
W3D_BACKEND_OPEN
    bool IsEnabled(Cap c);
W3D_BACKEND_CLOSE

    // leer pixeles (render-a-imagen / picking por color). ReadPixelsBegin avisa ANTES de dibujar que zona se va a
    // leer: donde no se puede leer el framebuffer (la PVR de la Dreamcast arma el cuadro recien al cerrarlo), lo que
    // se dibuja hasta el ReadPixelsRGBA va por software a esa zona. En el resto no hace nada.
W3D_BACKEND_OPEN
    void ReadPixelsBegin(int x, int y, int w, int h);
    void ReadPixelsRGBA(int x, int y, int w, int h, unsigned char* pixels);
W3D_BACKEND_CLOSE

    // mezcla alfa estandar: SRC_ALPHA, ONE_MINUS_SRC_ALPHA (para Blend)
W3D_BACKEND_OPEN
    void BlendAlpha();
    void BlendMode(int mode); // capa multi-pass (W3dLayerMix): 0=Mix(alpha encima), 1=Multiply, 2=Add,
W3D_BACKEND_CLOSE
                              // 3=Multiply 2x (dst*src*2, lightmaps con overbright), 4=Subtract, 5=Screen
                              // 6=Multiply con FACTOR (DST_COLOR, SRC_ALPHA: dst * (color*tex + alfa)); lo usa la capa
                              //   de textura con su factor de mezcla (Mesh.cpp)

    // --- MODOS DE MEZCLA (blend) ---
    // Todos con glBlendFunc estandar -> andan en GL ES 1.1 (N95), PC y WebGL. El unico que
    // necesita glBlendEquation es Substract (nativo en PC/WebGL; en GL ES 1.1 cae a una
    // aproximacion que oscurece). Acordate de Enable(Blend) antes de dibujar.
    enum MixMode {
        MixOff,       // opaco (ONE, ZERO)
        MixAlpha,     // normal:    SRC_ALPHA, ONE_MINUS_SRC_ALPHA
        MixAdd,       // aditiva:   ONE, ONE                 (aclara: brillos, fuego, particulas)
        MixAddAlpha,  // aditiva ponderada por alpha: SRC_ALPHA, ONE
        MixMultiply,  // multiplica: DST_COLOR, ZERO         (oscurece: tintes, sombras)
        MixScreen,    // screen:    ONE, ONE_MINUS_SRC_COLOR (aclara suave)
        MixPremult,   // alpha premultiplicado: ONE, ONE_MINUS_SRC_ALPHA
        MixSubtract,  // resta:     REVERSE_SUBTRACT / aprox en GL ES 1.1 (oscurece)
        MixCount_     // (cantidad; dejalo ultimo)
    };
    // BLUR VERTICAL por shader (motion blur de rodillos, etc). 'amt' en fraccion de la textura
    // (0 = apagado). SOLO tiene efecto en el backend GL ES 2.0 / WebGL (shaders); en el pipeline
    // fijo (desktop GL / GL ES 1.1 / N95) es un no-op silencioso.
W3D_BACKEND_OPEN
    void BlurY(float amt);
W3D_BACKEND_CLOSE

    // DESCARTE POR ALPHA: los pixeles con alpha < ref no se dibujan NI escriben en el z-buffer
    // (ref = 0 lo apaga). Sirve, por ejemplo, para dibujar con z-buffer cosas con bordes
    // recortados (esquinas redondeadas): por el agujero se sigue viendo lo de atras.
    // GLES2 -> 'discard' en el shader. Fija -> GL_ALPHA_TEST.
W3D_BACKEND_OPEN
    void AlphaTest(float ref);
W3D_BACKEND_CLOSE

    // VIDEO CON ALPHA EMPAQUETADO: la textura trae el COLOR en la mitad de arriba y la MASCARA
    // (en gris) en la mitad de abajo. Sirve para reproducir animaciones con transparencia en
    // plataformas cuyo decodificador de video no entrega canal alpha a la textura (Safari/iOS con
    // VP9-alpha, por ejemplo). Solo tiene efecto en el backend con shaders (GLES2/WebGL).
W3D_BACKEND_OPEN
    void AlphaPacked(bool activated);
W3D_BACKEND_CLOSE

W3D_BACKEND_OPEN
    void SetMix(int m);          // configura glBlendFunc/Equation segun el modo
W3D_BACKEND_CLOSE

    // --- parametros de la textura 2D ACTIVA (la del ultimo BindTexture) ---
W3D_BACKEND_OPEN
    void TexFilter(bool linear); // true=LINEAR, false=NEAREST (min y mag)
    void TexWrap(bool repeat);   // true=REPEAT, false=CLAMP_TO_EDGE (S y T)
W3D_BACKEND_CLOSE

    // --- PIXELADO GLOBAL (opcion de ESCENA, no de material) ---
    // Reporte del dueno: "pixela todas las texturas y no uses filtros de textura".
    // Hasta hoy eso era un flag POR MATERIAL (`filtrado`), y encima la palabra
    // PIXELATED del .mtl escribia `Material::interpolacion`, que NADIE leia al
    // dibujar: el flag no hacia absolutamente nada.
    // Con esto en true, para TODA la escena 3D:
    //   * NEAREST en minificacion Y magnificacion (Mesh::ApplyMaterial ignora
    //     `mat->filtered`),
    //   * SIN mipmaps (w3dEngine::LoadTexture no arma la piramide -> ademas se
    //     ahorra ~33 % de bytes de GPU por textura),
    //   * sin anisotropico (el motor nunca lo pidio: no hay glTexParameter de
    //     GL_TEXTURE_MAX_ANISOTROPY_EXT en ningun backend).
    // NO toca la UI 2D (fuente/iconos/tarjetas): esos siguen con su `filtrado`
    // por elemento, o el texto del editor saldria dentado.
    // OJO: se lee al SUBIR la textura, asi que cambiarlo con texturas ya
    // cargadas solo cambia el filtro (los mipmaps ya subidos quedan hasta la
    // proxima carga). El .w3d lo declara con "pixelado: true" en su cabecera.
    bool PixelatedGlobal();
    void SetPixelatedGlobal(bool on);

    // MIPMAPPING GLOBAL (default true; el .w3d lo apaga con "mipmaps: false").
    // Solo las texturas POT suben piramide; TexFilter elige el min-filter mipmap
    // (NEAREST_MIPMAP_LINEAR con pixelado) SOLO para las que la tienen
    // (TexHasMips), porque una sin piramide con ese filtro queda incompleta.
    // Igual que el pixelado, se lee al SUBIR: cambiarlo no re-arma lo ya cargado.
    bool MipmapsGlobal();
    void SetMipmapsGlobal(bool on);
    // con mipmaps: mezclar los dos niveles mas cercanos (trilineal, por defecto) o usar solo el mas cercano (en el MBX
    // del N95 lee la mitad de texeles)
    bool MipmapsTrilinear();
    void SetTrilinearMipmaps(bool on);
    bool TexHasMips(unsigned int id);
W3D_BACKEND_OPEN
    void TexBaseLevel(int level);   // ver un nivel puntual de la piramide (0 = normal; PC solamente)
W3D_BACKEND_CLOSE

    // CACHE de parametros POR TEXTURA (glTexParameter = estado del OBJETO textura,
    // no del contexto: re-mandar el mismo filtro/wrap por malla era churn puro).
    // Devuelven true si la firma CAMBIO (hay que llamar al driver). Viven en
    // w3dTexture.cpp junto al registro de mips; DeleteTexture olvida la entrada.
    bool TexFilterChanges(unsigned int id, int signature);
    bool TexWrapChanges(unsigned int id, int signature);
    void TexParamsRegister(unsigned int id, int signatureFilter, int signatureWrap);
    void TexParamsForget(unsigned int id);

    // --- punteros de los arrays del pipeline fijo (stride en BYTES) ---
W3D_BACKEND_OPEN
    void VertexPointer2f(int strideBytes, const float* p);         // 2 float (UI 2D)
    void VertexPointer3f(int strideBytes, const float* p);         // 3 float
    void VertexPointer4f(int strideBytes, const float* p);         // 4 float (x*w, y*w, z*w, w: 2D con perspectiva en las UV)
    void VertexPointer3s(int strideBytes, const short* p);         // 3 short (verts enteros)
    void VertexPointer2s(int strideBytes, const short* p);         // 2 short (UI 2D entera)
    void ColorPointer4ub(const unsigned char* p);                  // 4 ubyte
    void ColorPointer4f(int strideBytes, const float* p);          // 4 float (GLdc: su camino rapido es todo float)
    void NormalPointer3b(const signed char* p);                    // 3 byte
    void TexCoordPointer2f(int strideBytes, const float* p);       // 2 float
    void TexCoordPointer2s(int strideBytes, const short* p);       // 2 short (enteros: la escala, en la TextureMatrix)
    void TexCoordPointer3b(const signed char* p, int count);       // 3 byte (normales como texcoords, matcap HW). count=vertices (desktop convierte a GL_SHORT)
W3D_BACKEND_CLOSE
#ifdef W3D_DREAMCAST
    // GLdc define GLbyte como 'char' (no 'signed char'): las normales en bytes de las mallas llegan como char*
    inline void NormalPointer3b(const char* p) { NormalPointer3b((const signed char*)p); }
    inline void TexCoordPointer3b(const char* p, int count) { TexCoordPointer3b((const signed char*)p, count); }
#endif

    // --- BUFFER OBJECTS (VBO/IBO): subir la malla UNA vez a memoria de GPU y dibujar desde ahi, en vez de
    //     re-transferir los client-arrays de RAM en CADA glDrawElements. GL ES 1.1 (N95/MBX) y GL ES 2 (WebGL) los
    //     traen en el core; en desktop GL 1.1 (Windows) se cargan via wglGetProcAddress. handle 0 = ninguno.
    //     La malla ESTATICA sube 1 vez cuando cambia la geometria -> gran salto de FPS (sobre todo en el MBX). ---
W3D_BACKEND_OPEN
    bool VBOSupported();                                          // true si el backend/driver tiene buffer objects
    unsigned int GenBuffer();                                     // crea un buffer object (0 si falla / no soportado)
    void DeleteBuffer(unsigned int h);                            // lo libera
W3D_BACKEND_CLOSE
    int LiveBuffers();                                           // los creados y no borrados (fugas)
W3D_BACKEND_OPEN
    void ArrayBufferData(unsigned int h, const void* data, int bytes);  // sube atributos a GL_ARRAY_BUFFER (STATIC_DRAW)
    void IndexBufferData(unsigned int h, const void* data, int bytes);  // sube indices a GL_ELEMENT_ARRAY_BUFFER (STATIC_DRAW)
    long VBOMemory();                                            // bytes de los buffer objects vivos (lo ultimo subido a cada uno)
W3D_BACKEND_CLOSE
    // setup del draw DESDE VBOs (cada atributo en su propio buffer -> puntero con offset 0):
W3D_BACKEND_OPEN
    void VertexVBO(unsigned int h);    // bind + glVertexPointer(3,FLOAT,0,0)
    void NormalVBO(unsigned int h);    // bind + glNormalPointer(BYTE,0,0)
    void ColorVBO(unsigned int h);     // bind + glColorPointer(4,UBYTE,0,0)
    void TexCoordVBO(unsigned int h);  // bind + glTexCoordPointer(2,FLOAT,0,0)
W3D_BACKEND_CLOSE
    // los mismos con ENTEROS (3 / 2 short): la mitad de memoria y, en el N95 (el MBX sin VGP transforma en la CPU),
    // menos trabajo para el driver. La escala va en la matriz (ModelView / TextureMatrix)
W3D_BACKEND_OPEN
    void VertexVBO3s(unsigned int h);  // bind + glVertexPointer(3,SHORT,0,0)
    void TexCoordVBO2s(unsigned int h);// bind + glTexCoordPointer(2,SHORT,0,0)
    void BindIndexVBO(unsigned int h); // bind del IBO (GL_ELEMENT_ARRAY_BUFFER)
    void DrawTrianglesVBO(int count, int firstIndex); // glDrawElements desde el IBO bindeado (offset = firstIndex indices)
W3D_BACKEND_CLOSE
    // (P3) INDICES DE CLIENTE sobre ATRIBUTOS EN VBO: el subconjunto de indices
    // viaja por el bus en el draw (como la PSX mandaba su display list) y los
    // atributos quedan ESTATICOS en GPU. Es el camino de la visibilidad por
    // triangulo: cambiar de celda/sector NO re-especifica ningun buffer (la
    // leccion del stall del tiler MBX ya esta pagada; glBufferData sobre un
    // buffer que el tiler todavia lee = sync stall). Desbindea el ELEMENT_ARRAY
    // (los indices se leen de RAM) y deja los atributos como esten. Suma los
    // bytes de indices a g_statUploadBytes (es LO que cruza el bus por frame).
W3D_BACKEND_OPEN
    void DrawTrianglesClientIdx(int count, const MeshIndex* indices);
W3D_BACKEND_CLOSE
    // el gemelo en GL_LINES: aristas de un material con `lineas` sobre los
    // atributos que esten bindeados (VBO o client arrays; mismo contrato).
W3D_BACKEND_OPEN
    void DrawLinesClientIdx(int count, const MeshIndex* indices);
    void UnbindVBOs();                 // vuelve a client-side arrays (bind ARRAY/ELEMENT a 0; perezoso con g_pointersCache)
W3D_BACKEND_CLOSE
    // CACHE DE PUNTEROS Y DE BINDS (pipeline fijo): un puntero (RAM o VBO) igual al que GL ya tiene no llama al driver
    // (ni al bind que lo precede: GL guarda con cada puntero el buffer que estaba bindeado), BindIndexVBO del IBO ya
    // bindeado tampoco, y UnbindVBOs no hace nada: el 0 se bindea recien antes de un puntero de RAM (ARRAY_BUFFER) o de
    // un draw con indices de cliente (ELEMENT_ARRAY_BUFFER). Todo puntero y todo bind de buffer tiene que pasar por
    // aca (el GL crudo no se ve); Invalidate y DeleteBuffer olvidan lo que corresponde
    extern bool g_pointersCache;       // default true; false = como antes (A/B: harness 'punteroscache')

    // --- PRIMITIVAS de GL ES 1.1 (la Rutina: "dibujar elementos" con la primitiva que se elija). QUADS,
    //     QUAD_STRIP y POLYGON no existen en GL ES: los SIMULA quien llama, pasando los indices ya convertidos a
    //     triangulos (mas indices, mas lento). ---
    enum Primitive { PrimPoints = 0, PrimLines, PrimStripLines, PrimLoopLines, PrimTriangles, PrimStripTriangles,
                     PrimFan };
W3D_BACKEND_OPEN
    void DrawElements(Primitive p, int count, const MeshIndex* indices);   // indices en RAM (client-side)
W3D_BACKEND_CLOSE
    // indices de 16 bits en RAM, en todas las plataformas (MeshIndex es de 32 bits fuera del N95): un juego que ya
    // trae sus indices en 16 bits (la Dreamcast: GLdc los lee directo) no los convierte ni los recorre
W3D_BACKEND_OPEN
    void DrawElements16(Primitive p, int count, const unsigned short* indices);
    void DrawElementsVBO(Primitive p, int count, int firstIndex);          // del IBO bindeado (offset en indices)
W3D_BACKEND_CLOSE

    // dibuja triangulos indexados (indices ushort)
W3D_BACKEND_OPEN
    void DrawTriangles(int count, const MeshIndex* indices); // index buffer 16/32 bits (ver crossplatform.h)
    void DrawTrianglesArray(int vertexCount); // glDrawArrays(GL_TRIANGLES): la UI 2D no indexa
    void DrawTrianglesArrayFrom(int first, int count); // glDrawArrays con offset (chrome equirect por-corner)
    void DrawTrianglesByte(int count, const unsigned char* indices); // glDrawElements(GL_TRIANGLES) con indices ubyte
    void DrawLines(int vertexCount); // glDrawArrays(GL_LINES): overlay de normales / bordes
    void DrawLineStrip(int vertexCount); // glDrawArrays(GL_LINE_STRIP): curvas
    void DrawLineStripIndexed(int count, const unsigned short* indices); // glDrawElements(GL_LINE_STRIP)
    void DrawPoints(int vertexCount); // glDrawArrays(GL_POINTS): vertices en Edit Mode
    void PointSize(float px);          // glPointSize (tamano del punto)
    void DrawLinesIndexed(int count, const unsigned short* indices); // glDrawElements(GL_LINES)
    void DepthRange(float n, float f); // empuja el zbuffer (contorno de seleccion)
    void PolygonOffset(float factor, float units); // glPolygonOffset (slope-aware)
W3D_BACKEND_CLOSE

    // wireframe (relleno por lineas). Solo GL de escritorio tiene glPolygonMode;
    // en GL ES es no-op (la malla sale rellena, como hoy en Symbian/Android).
W3D_BACKEND_OPEN
    void Wireframe(bool on);
W3D_BACKEND_CLOSE

    // --- cache de estado ---
    // Enable/Disable, BindTexture Y el estado fino (depth func/mask, blend,
    // texenv, texgen/matcap, shade model, alpha test, client arrays) llevan
    // cache: cambiar a un estado que YA esta puesto no genera llamada GL. OJO:
    // el cache solo es correcto si TODO el estado pasa por el motor. Si algun
    // codigo todavia llama GL directo hay que llamar Invalidate() al volver al
    // motor para resincronizar. El pase de ESCENA se resincroniza UNA vez por
    // frame (antes del traversal), no por malla: nada de lo que corre adentro
    // del pase puede tocar GL crudo (lo vigila AuditState en debug).
W3D_BACKEND_OPEN
    void Invalidate();
W3D_BACKEND_CLOSE
    // el estado de MATERIAL que la escena (una rutina, una malla) puede dejar puesto y lo que se dibuja despues
    // (overlays, HUD, paneles) no repone: alpha test, sesgo de poligonos, matriz y modo de textura, sombreado, luz y
    // color de vertice. Por el cache: si ya esta asi, no llama al driver. Lo llama el cierre del pase de escena
W3D_BACKEND_OPEN
    void BaseState();
W3D_BACKEND_CLOSE
    // FOTO del estado cacheado (el CONSTRUCTOR de una Rutina en modo solo-juego corre UNA vez): Tomar guarda lo que
    // el motor cree puesto (caps, textura, z, mezcla, texenv, alpha, arrays, material, color, polygon offset);
    // Reponer lo vuelve a poner PASANDO POR EL CACHE: solo llama al driver por lo que algo cambio desde la foto
    // (el HUD 2D de cada cuadro, por ejemplo). Los parametros que el cache no lleva (niebla, luces, clear color)
    // siguen puestos si nadie los toca. SnapshotStateSupported() = false en un backend sin cache (GLES2): ahi el
    // constructor corre en cada cuadro.
W3D_BACKEND_OPEN
    bool SnapshotStateSupported();
    void SnapshotStateTake();
    void SnapshotStateRestore();
W3D_BACKEND_CLOSE
    // auditoria del cache (PC/debug): compara el cache contra GL real via
    // glIsEnabled/glGet (flush en tilers: JAMAS por frame en dispositivo).
    // Devuelve la cantidad de desincronizaciones (0 = coherente) y las loguea.
W3D_BACKEND_OPEN
    int  AuditState();
W3D_BACKEND_CLOSE
    // el harness la prende ('glaudit'): el pase de escena audita al cerrar cada
    // frame y acumula los desyncs aca. Default off (cero costo).
    extern bool g_auditScene;
    extern int  g_auditDesyncs;

    // --- ESTADISTICAS de frame (bench/overlay): contadores baratos que acumulan
    //     por frame. StatsReset() los pone en 0 (lo llama el que mide, ej. el
    //     comando 'bench' del harness); se leen directo tras dibujar. ---
    extern int g_statDrawTris;   // draw calls de TRIANGULOS (DrawTriangles*/VBO/Array)
    extern int g_statDrawVBO;    // de esos, cuantos salieron del IBO en GPU (DrawTrianglesVBO)
    extern int g_statIndices;    // indices/vertices mandados en esos draws (~verts transformados)
    extern int g_statTexBinds;   // glBindTexture REALES (los cacheados no cuentan)
    extern int g_statMeshes;     // mallas que entraron a Mesh::RenderObject (lo incrementa Mesh.cpp)
    // --- presupuesto de render MEDIBLE (comando 'presupuesto' del harness): ---
    // cambios de ESTADO que llegaron al driver (Enable/Disable reales, arrays de
    // cliente, depth func/mask, blend func, texenv, texgen/matriz de matcap, alpha
    // test). Los ahorrados por el cache NO cuentan: este numero ES el churn.
    extern int g_statStateChanges;
    // llamadas al driver que los otros contadores NO ven: matrices (push/pop/mult/load/trasladar/rotar/escalar/
    // modo), lecturas de matriz (glGet), punteros de arrays y binds de VBO. Solo el pipeline fijo (PC / N95)
    extern int g_otherStat;
    extern int g_statUploadBytes;  // bytes subidos a buffer objects (VBO/IBO) este frame
    extern int g_opaqueStatTris;   // triangulos dibujados con Blend APAGADO
    extern int g_statTrisBlend;    // triangulos dibujados con Blend PRENDIDO
    // desglose por CATEGORIA: el pase que dibuja declara la suya (StatScope). El cuadro arranca en Editor (la interfaz
    // del editor), el pase de escena es Escena, el flush de particulas se marca solo y el HUD del juego es UI.
    enum StatCat { StatCatScene = 0, StatCatParticles, StatCatUI, StatCatEditor, StatCatCount_ };
    extern int g_statDcCat[StatCatCount_];   // draws de triangulos por categoria
W3D_BACKEND_OPEN
    void StatCategory(StatCat c);
    StatCat StatCurrentCategory();
    struct StatScope {                       // cambia la categoria en un bloque y al salir la repone
        StatCat prev;
        explicit StatScope(StatCat c) : prev(StatCurrentCategory()) { StatCategory(c); }
        ~StatScope() { StatCategory(prev); }
    };
    void StatsReset();
W3D_BACKEND_CLOSE
    // TODAS las llamadas al driver (cada gl* = 1, tambien las que los contadores de arriba no ven: clear, viewport,
    // scissor, color, niebla, lineas...) por categoria, en el cuadro que esta corriendo. El GL crudo que queda fuera
    // del motor (el layout de Symbian, el cursor) se suma con StatRaw
    extern int g_statCalls[StatCatCount_];
W3D_BACKEND_OPEN
    void StatRaw(int n);
W3D_BACKEND_CLOSE
    // la TRAZA (medir): con g_traceGL cada llamada suma en su texto ("glBindTexture(...)"); TraceGLVer la imprime
    // ordenada de mas a menos y la vacia. Solo el backend de pipeline fijo
    extern bool g_traceGL;
    extern bool g_traceGLSec;
    // EL ALFA DE TODO LO QUE SE PINTA: Color4f / Color4fv / Color4ub multiplican su alfa por este (1 = normal). La UI lo
    // baja para dibujar algo entero translucido (la tarjeta de la ultima operacion mientras se arrastra un valor: se ve
    // detras como queda el cambio) y lo vuelve a 1. Lo que ya estaba pintado no cambia: cuenta desde el proximo color.
    extern float g_alphaColor;
W3D_BACKEND_OPEN
    void TraceGL(const char* call);
    void TraceGLVer();
W3D_BACKEND_CLOSE
    // GL CRUDO fuera del motor (las luces, el layout y el cursor de Symbian, el video): W3D_GL_RAW(glXxx(...)) la
    // cuenta (mejor pasarlo por el motor: el cache de estado no lo ve)
    #define W3D_GL_RAW(call) (w3dEngine::StatRaw(1), (call))
    // el ULTIMO CUADRO completo (escena + HUD del juego + interfaz del editor): lo que muestran el overlay, statsvivo y
    // el [PERF]. StatsFrame() lo cierra (al empezar cada cuadro: W3dProfBegin, W3dGameRender)
    struct FrameStats {
        int calls[StatCatCount_];
        int total, draws, binds, states, tris;
        unsigned frames;
        // (con g_measureDraws, el backend de GLES2) cuanto tardo la CPU adentro de los draws y el mas lento: sus indices,
        // los vertices que leyo y si eran de un VBO
        float msDraws, msDrawMax;
        int drawMaxIdx, drawMaxVerts, drawMaxVBO;
        FrameStats() : total(0), draws(0), binds(0), states(0), tris(0), frames(0), msDraws(0), msDrawMax(0),
                          drawMaxIdx(0), drawMaxVerts(0), drawMaxVBO(0) {
            for (int i = 0; i < StatCatCount_; i++) calls[i] = 0;
        }
    };
    extern FrameStats g_statFrame;
W3D_BACKEND_OPEN
    void StatsFrame();
W3D_BACKEND_CLOSE
    extern bool g_measureDraws;   // medir el tiempo de cada draw (el medidor del juego compilado en modo debug)
W3D_BACKEND_OPEN
    void Finish();               // glFinish: drena el pipeline (medicion real de GPU en el bench)
W3D_BACKEND_CLOSE
    // glFlush: que la GPU empiece con lo mandado sin esperarla. En un PowerVR (el N95) el cuadro se rasteriza recien al
    // presentar: llamandola al terminar de dibujar, la CPU puede ir preparando el cuadro siguiente mientras tanto
W3D_BACKEND_OPEN
    void Flush();
W3D_BACKEND_CLOSE
    // lo que el backend tenga juntado sin dibujar (el lote de la UI de GLES2 en la VideoCore IV) se dibuja YA: antes del
    // swap y antes de tocar una textura o el framebuffer con GL crudo (los backends que no juntan no hacen nada)
W3D_BACKEND_OPEN
    void DrawPending();
W3D_BACKEND_CLOSE

    // Backend ES2/WebGL: la app lo llama UNA vez tras crear el contexto GL para cargar las
    // funciones GL2.0 (getProc = SDL_GL_GetProcAddress) y compilar el shader. Solo lo define
    // el backend gles2 (w3dGraphicsGLES2.cpp); el backend de pipeline fijo no lo usa.
    void GLES2Init(void* (*getProc)(const char*));
    // Backend ES2: alguien toco GL DIRECTO (viewport, scissor, la textura atada, las capacidades) sin pasar por el motor.
    // Con un programa por draw (las GPU chicas: el N8, la VideoCore IV) el backend recuerda ese estado para no pedirlo
    // de nuevo, y el GL crudo lo deja desincronizado: un viewport o un scissor que "ya estaba" no se vuelve a poner y
    // se dibuja donde no va (los menus flotantes del N8 arruinaban toda la pantalla). Esto lo vuelve a poner en regla.
    void GLES2Resync();
    // true SOLO durante el render FINAL (la foto/los pases): los objetos con
    // renderizable=false se saltean (en el viewport se ven igual)
    extern bool w3dRenderFinal;
    // dibujando las caras de una SONDA DE REFLEJO de tiempo real: lo que no sale en las sondas (Object::inProbes) no se dibuja
    extern bool w3dPassProbe;
    // LO DIBUJADO QUE LA GPU PERDIO: sube cada vez que un draw fallo y se llevo lo que ya habia en la imagen (la VideoCore
    // IV: el primer draw de un programa que no le entra falla con GL_OUT_OF_MEMORY y se pierde lo anterior del cuadro).
    // El render a archivo lo mira por franja y la dibuja de nuevo (ViewPort3D, el bucle de las franjas)
    extern unsigned w3dDrawsLost;

    // PASE DE ESPEJO: true mientras el Mirror dibuja el reflejo. El material de
    // la malla NO puede decidir el z-test durante ese pase (el espejo ya lo
    // fijo). Sin esto, Mesh::ApplyMaterial hacia Enable(DepthTest) desde
    // mat->depth_test y DESHACIA el 'noDepth' del espejo: el reflejo,
    // que queda mas hondo que el fondo opaco del estanque, no se veia nunca.
    extern bool w3dPassMirrorNoZ;
}

// Estado de render (lo setea la app/editor antes de dibujar la escena; el Core lo LEE para
// dibujar segun el modo). El editor lo deriva de su RenderType/view: el Core NO conoce esos modos.
extern bool w3dRenderWireframe; // dibuja wireframe en vez del relleno
// wireframe SOLO de lo que se dibuja (default true): con visibilidad por triangulo
// activa (Mesh::pvsFaces, modificador Culling) las lineas se arman de la LISTA
// ACTIVA y no de todas las aristas. En false, el wireframe clasico (todo).
extern bool w3dRenderWireframeOnlyVisible;
extern bool w3dRenderSolid;      // material por defecto sin texturas (modo Solid)
extern bool w3dRenderNoLight;    // sin iluminacion, solo profundidad (modo ZBuffer)
extern bool w3dRenderLights;     // aplicar las luces de escena (por defecto si: el render; el editor la apaga en sus
                                // modos de vista que usan otra luz)
extern bool w3dRenderNormalColor; // dibuja la malla unlit con color = normal (debug de normales; modo Normal View)
extern bool w3dRenderAlpha;       // pase ALPHA (matte): blanco unlit + solo el alpha de la textura (sin fog)
extern bool w3dRenderOverlays;    // el editor quiere overlays (contorno de seleccion / overlay de edit); el Core lo LEE
extern bool w3dShowSelection;     // "Ver seleccion" (menu Select del viewport): OFF = sin contorno NI tinte de seleccion
extern bool g_xray;               // X-Ray (overlay): la malla EN EDICION se dibuja semitransparente sin z-test (retopo)
