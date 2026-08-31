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
    void Enable(Cap c);
    void Disable(Cap c);

    // comparacion del depth test
    //  DepthEqual : pasadas extra sobre la MISMA superficie (normal map, capas)
    //  DepthAlways: ESCRIBIR profundidad a la fuerza. Lo usa el espejo para
    //               "borrar" la z adentro de la mascara de estencil. OJO: NO se
    //               puede hacer con Clear(DepthBuffer), porque el glClear NO pasa
    //               por el test de estencil (solo por el scissor y los writemasks,
    //               spec GL 2.1 §4.2.3): un Clear ahi borraria la profundidad de
    //               TODA la ventana y el agua se dibujaria encima de todo.
    enum DepthCmp { DepthLess, DepthLEqual, DepthEqual, DepthAlways };
    void DepthFunc(DepthCmp c);

    // --- arrays de vertices del pipeline fijo ---
    enum VArray { VertexArray, TexCoordArray, NormalArray, ColorArray };
    void EnableArray(VArray a);
    void DisableArray(VArray a);

    // --- buffers ---
    void ClearColor(float r, float g, float b, float a);
    enum { ColorBuffer = 1, DepthBuffer = 2, StencilBuffer = 4 };
    void Clear(int bits); // ColorBuffer | DepthBuffer | StencilBuffer

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
    // en el shader (los primeros PlanosRecorteMax()).
    int  PlanosRecorteMax();                        // cuantos soporta el backend/driver
    void PlanosRecorte(int n, const float* eq4);    // n planos, 4 floats cada uno

    // --- ESTENCIL: recorte por FORMA ---
    // La unica forma de acotar una pasada a algo que NO es un rectangulo de
    // pantalla ni un semiespacio: se MARCA una silueta (dibujandola con z-test,
    // asi lo que este DELANTE no la marca) y despues se dibuja solo adentro de
    // la marca. El espejo del agua lo usa para que el reflejo quede recortado a
    // la lamina de agua REALMENTE VISIBLE (si un puente tapa el estanque, el
    // reflejo no se pinta encima del puente).
    // EstencilBits() = 0 cuando el contexto se creo sin buffer de estencil: el
    // que llama tiene que poder seguir sin el (el espejo cae al recorte por planos).
    int  EstencilBits();
    void EstencilLimpiar(int valor);   // pone TODO el buffer en 'valor'
    void EstencilMarcar(int valor);    // test siempre pasa y ESCRIBE 'valor' donde se dibuje
    void EstencilDentro(int valor);    // dibuja SOLO donde el buffer vale 'valor' (no escribe)
    void EstencilApagar();             // desactiva el test (y deja el buffer sin escribir)

    // --- viewport y recorte (coordenadas ya en el sistema del backend) ---
    void Viewport(int x, int y, int w, int h);
    void Scissor(int x, int y, int w, int h);

    // --- matrices del pipeline fijo ---
    enum Matrix { Projection, ModelView, TextureMatrix };
    void MatrixMode(Matrix m);
    void LoadIdentity();

    // pila de matrices + traslacion (la UI 2D posiciona asi). El backend GL las hace con
    // glPushMatrix/glTranslatef; un backend Vulkan/DX las haria con Matrix4 explicita.
    void PushMatrix();
    void PopMatrix();
    void Translatef(float x, float y, float z);
    void Rotatef(float angleDeg, float x, float y, float z); // rotacion sobre un eje
    void Scalef(float x, float y, float z);
    void MultMatrix(const float* m16);  // multiplica la matriz actual por m (column-major)
    void LoadMatrix(const float* m16);   // carga m como matriz actual (ej: la vista de CameraBase)

    // proyecciones (cargan/multiplican la matriz PROJECTION activa)
    void Ortho(float l, float r, float b, float t, float n, float f);
    void Frustum(float l, float r, float b, float t, float n, float f);
    void Perspective(float fovYDeg, float aspect, float n, float f); // via Frustum (sin GLU)

    // --- estado suelto ---
    void DepthMask(bool escribir);     // permitir/escribir el z-buffer
    void LineWidth(float px);
    void SmoothShading(bool suave);    // GL_SMOOTH (true) / GL_FLAT (false)
    void FastPerspective();            // hint de correccion de perspectiva rapida
    void FrontFace(bool ccw);          // winding de la cara frontal (true=CCW)
    void ColorMask(bool r, bool g, bool b, bool a); // permitir/bloquear canales de color
    void PointSpriteCoordReplace(bool on); // genera coords de textura por punto (point sprites)

    // --- textura activa (con CACHE: ver abajo) ---
    // Bindea la textura 2D. El motor recuerda cual esta puesta: si ya es esa,
    // NO hace la llamada GL (ahorra el bind, que es caro). id 0 = ninguna.
    void BindTexture(unsigned int id);

    // CHROME (env-map esferico / matcap): genera las UV desde el normal -> reflejo falso. PC = GL_SPHERE_MAP
    // del pipeline fijo (hardware). Symbian/GLES1 NO tiene glTexGen: queda como stub (UV por software TODO).
    void TexGenSphere(bool on);

    // MATCAP por HARDWARE sin texgen (anda en GLES1/N95): arma la matriz de TEXTURA = rotacion del modelview x
    // escala/bias (+ flip-V), y las NORMALES se mandan como texcoords (TexCoordPointer3b) -> la GPU calcula la UV
    // del matcap por vertice. on=arma la matriz; off=la resetea a identidad. (Es el "normal del ojo", aprox del
    // GL_SPHERE_MAP exacto.) NO mezclar con TexGenSphere (los dos tocan la matriz de textura).
    void TexMatrixMatcap(bool on);

    // true si hay glTexGen (sphere-map EXACTO por hardware: PC). false en GLES1 (N95) -> ese modo cae a software.
    bool TieneTexGen();

    // chrome = espejo: textura DIRECTA (GL_REPLACE), independiente de la luz. false = GL_MODULATE normal.
    // Lo usan los dos modos de chrome (matcap y equirect). Anda en PC y GLES1 (glTexEnvi existe en ambos).
    void TexEnvReplace(bool on);
    void TexEnvDot3(bool on); // DOT3 normal mapping: combiner N.L (textura=N, color=L). Reset=MODULATE
    void TexEnvAlphaOnly(bool on); // pases planos: RGB=color de vertice, ALPHA=alpha de la textura. Reset=MODULATE

    // --- color uniforme del pipeline fijo (cuando NO hay array de color) ---
    void Color4f(float r, float g, float b, float a);
    void Color4fv(const float* rgba);
    void Color4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a);

    // --- MATERIAL del pipeline fijo ---
    // El motor resuelve float (GL escritorio / GL ES 1.1 de Symbian) vs punto
    // fijo (GL ES 1.0 de Android viejo, glMaterialx*) adentro: quien lo use
    // pasa siempre float y NO necesita #ifdef ANDROID. Cara = FRONT_AND_BACK.
    enum MatParam { MatAmbient, MatDiffuse, MatSpecular, MatEmission };
    void Material(MatParam p, const float* rgba); // 4 floats
    void MaterialShininess(float s);

    // --- niebla (fog) del pipeline fijo ---
    void FogMode(bool linear);          // GL_FOG_MODE: true=LINEAR
    void FogStart(float z);
    void FogEnd(float z);
    void FogColor(const float* rgba);

    // --- luz 0 del pipeline fijo ---
    enum LightFv { LightAmbient, LightDiffuse, LightSpecular, LightPosition };
    void Light0fv(LightFv p, const float* v);
    enum LightF { LightConstantAtt, LightLinearAtt, LightQuadraticAtt };
    void Light0f(LightF p, float v);
    void SetLightEnabled(unsigned int glLightId, bool on); // habilita/apaga una luz por su id GL

    // query: usa el CACHE del motor (NO llama glIsEnabled, que flushea el GPU en el N95)
    bool IsEnabled(Cap c);

    // leer pixeles (render-a-imagen / picking por color)
    void ReadPixelsRGBA(int x, int y, int w, int h, unsigned char* pixels);

    // mezcla alfa estandar: SRC_ALPHA, ONE_MINUS_SRC_ALPHA (para Blend)
    void BlendAlpha();
    void BlendMode(int modo); // capa multi-pass: 0=Mix(alpha encima), 1=Multiply, 2=Add

    // --- MODOS DE MEZCLA (blend) ---
    // Todos con glBlendFunc estandar -> andan en GL ES 1.1 (N95), PC y WebGL. El unico que
    // necesita glBlendEquation es Substract (nativo en PC/WebGL; en GL ES 1.1 cae a una
    // aproximacion que oscurece). Acordate de Enable(Blend) antes de dibujar.
    enum Mezcla {
        MezclaOff,       // opaco (ONE, ZERO)
        MezclaAlpha,     // normal:    SRC_ALPHA, ONE_MINUS_SRC_ALPHA
        MezclaAdd,       // aditiva:   ONE, ONE                 (aclara: brillos, fuego, particulas)
        MezclaAddAlpha,  // aditiva ponderada por alpha: SRC_ALPHA, ONE
        MezclaMultiply,  // multiplica: DST_COLOR, ZERO         (oscurece: tintes, sombras)
        MezclaScreen,    // screen:    ONE, ONE_MINUS_SRC_COLOR (aclara suave)
        MezclaPremult,   // alpha premultiplicado: ONE, ONE_MINUS_SRC_ALPHA
        MezclaSubtract,  // resta:     REVERSE_SUBTRACT / aprox en GL ES 1.1 (oscurece)
        MezclaCount_     // (cantidad; dejalo ultimo)
    };
    // BLUR VERTICAL por shader (motion blur de rodillos, etc). 'amt' en fraccion de la textura
    // (0 = apagado). SOLO tiene efecto en el backend GL ES 2.0 / WebGL (shaders); en el pipeline
    // fijo (desktop GL / GL ES 1.1 / N95) es un no-op silencioso.
    void BlurY(float amt);

    // DESCARTE POR ALPHA: los pixeles con alpha < ref no se dibujan NI escriben en el z-buffer
    // (ref = 0 lo apaga). Sirve, por ejemplo, para dibujar con z-buffer cosas con bordes
    // recortados (esquinas redondeadas): por el agujero se sigue viendo lo de atras.
    // GLES2 -> 'discard' en el shader. Fija -> GL_ALPHA_TEST.
    void AlphaTest(float ref);

    // VIDEO CON ALPHA EMPAQUETADO: la textura trae el COLOR en la mitad de arriba y la MASCARA
    // (en gris) en la mitad de abajo. Sirve para reproducir animaciones con transparencia en
    // plataformas cuyo decodificador de video no entrega canal alpha a la textura (Safari/iOS con
    // VP9-alpha, por ejemplo). Solo tiene efecto en el backend con shaders (GLES2/WebGL).
    void AlfaEmpaquetado(bool activado);

    void SetMezcla(int m);          // configura glBlendFunc/Equation segun el modo

    // --- parametros de la textura 2D ACTIVA (la del ultimo BindTexture) ---
    void TexFilter(bool linear); // true=LINEAR, false=NEAREST (min y mag)
    void TexWrap(bool repeat);   // true=REPEAT, false=CLAMP_TO_EDGE (S y T)

    // --- PIXELADO GLOBAL (opcion de ESCENA, no de material) ---
    // Reporte del dueno: "pixela todas las texturas y no uses filtros de textura".
    // Hasta hoy eso era un flag POR MATERIAL (`filtrado`), y encima la palabra
    // PIXELATED del .mtl escribia `Material::interpolacion`, que NADIE leia al
    // dibujar: el flag no hacia absolutamente nada.
    // Con esto en true, para TODA la escena 3D:
    //   * NEAREST en minificacion Y magnificacion (Mesh::AplicarMaterial ignora
    //     `mat->filtrado`),
    //   * SIN mipmaps (w3dEngine::LoadTexture no arma la piramide -> ademas se
    //     ahorra ~33 % de bytes de GPU por textura),
    //   * sin anisotropico (el motor nunca lo pidio: no hay glTexParameter de
    //     GL_TEXTURE_MAX_ANISOTROPY_EXT en ningun backend).
    // NO toca la UI 2D (fuente/iconos/tarjetas): esos siguen con su `filtrado`
    // por elemento, o el texto del editor saldria dentado.
    // OJO: se lee al SUBIR la textura, asi que cambiarlo con texturas ya
    // cargadas solo cambia el filtro (los mipmaps ya subidos quedan hasta la
    // proxima carga). El .w3d lo declara con "pixelado: true" en su cabecera.
    bool PixeladoGlobal();
    void SetPixeladoGlobal(bool on);

    // MIPMAPPING GLOBAL (default true; el .w3d lo apaga con "mipmaps: false").
    // Solo las texturas POT suben piramide; TexFilter elige el min-filter mipmap
    // (NEAREST_MIPMAP_LINEAR con pixelado) SOLO para las que la tienen
    // (TexTieneMips), porque una sin piramide con ese filtro queda incompleta.
    // Igual que el pixelado, se lee al SUBIR: cambiarlo no re-arma lo ya cargado.
    bool MipmapsGlobal();
    void SetMipmapsGlobal(bool on);
    bool TexTieneMips(unsigned int id);
    void TexBaseLevel(int nivel);   // ver un nivel puntual de la piramide (0 = normal; PC solamente)

    // CACHE de parametros POR TEXTURA (glTexParameter = estado del OBJETO textura,
    // no del contexto: re-mandar el mismo filtro/wrap por malla era churn puro).
    // Devuelven true si la firma CAMBIO (hay que llamar al driver). Viven en
    // w3dTexture.cpp junto al registro de mips; DeleteTexture olvida la entrada.
    bool TexFiltroCambia(unsigned int id, int firma);
    bool TexWrapCambia(unsigned int id, int firma);
    void TexParamsRegistrar(unsigned int id, int firmaFiltro, int firmaWrap);
    void TexParamsOlvidar(unsigned int id);

    // --- punteros de los arrays del pipeline fijo (stride en BYTES) ---
    void VertexPointer2f(int strideBytes, const float* p);         // 2 float (UI 2D)
    void VertexPointer3f(int strideBytes, const float* p);         // 3 float
    void VertexPointer3s(int strideBytes, const short* p);         // 3 short (verts enteros)
    void VertexPointer2s(int strideBytes, const short* p);         // 2 short (UI 2D entera)
    void ColorPointer4ub(const unsigned char* p);                  // 4 ubyte
    void NormalPointer3b(const signed char* p);                    // 3 byte
    void TexCoordPointer2f(int strideBytes, const float* p);       // 2 float
    void TexCoordPointer3b(const signed char* p, int count);       // 3 byte (normales como texcoords, matcap HW). count=vertices (desktop convierte a GL_SHORT)

    // --- BUFFER OBJECTS (VBO/IBO): subir la malla UNA vez a memoria de GPU y dibujar desde ahi, en vez de
    //     re-transferir los client-arrays de RAM en CADA glDrawElements. GL ES 1.1 (N95/MBX) y GL ES 2 (WebGL) los
    //     traen en el core; en desktop GL 1.1 (Windows) se cargan via wglGetProcAddress. handle 0 = ninguno.
    //     La malla ESTATICA sube 1 vez cuando cambia la geometria -> gran salto de FPS (sobre todo en el MBX). ---
    bool VBOSoportado();                                          // true si el backend/driver tiene buffer objects
    unsigned int GenBuffer();                                     // crea un buffer object (0 si falla / no soportado)
    void DeleteBuffer(unsigned int h);                            // lo libera
    void ArrayBufferData(unsigned int h, const void* data, int bytes);  // sube atributos a GL_ARRAY_BUFFER (STATIC_DRAW)
    void IndexBufferData(unsigned int h, const void* data, int bytes);  // sube indices a GL_ELEMENT_ARRAY_BUFFER (STATIC_DRAW)
    // setup del draw DESDE VBOs (cada atributo en su propio buffer -> puntero con offset 0):
    void VertexVBO(unsigned int h);    // bind + glVertexPointer(3,FLOAT,0,0)
    void NormalVBO(unsigned int h);    // bind + glNormalPointer(BYTE,0,0)
    void ColorVBO(unsigned int h);     // bind + glColorPointer(4,UBYTE,0,0)
    void TexCoordVBO(unsigned int h);  // bind + glTexCoordPointer(2,FLOAT,0,0)
    void BindIndexVBO(unsigned int h); // bind del IBO (GL_ELEMENT_ARRAY_BUFFER)
    void DrawTrianglesVBO(int count, int firstIndex); // glDrawElements desde el IBO bindeado (offset = firstIndex indices)
    // (P3) INDICES DE CLIENTE sobre ATRIBUTOS EN VBO: el subconjunto de indices
    // viaja por el bus en el draw (como la PSX mandaba su display list) y los
    // atributos quedan ESTATICOS en GPU. Es el camino de la visibilidad por
    // triangulo: cambiar de celda/sector NO re-especifica ningun buffer (la
    // leccion del stall del tiler MBX ya esta pagada; glBufferData sobre un
    // buffer que el tiler todavia lee = sync stall). Desbindea el ELEMENT_ARRAY
    // (los indices se leen de RAM) y deja los atributos como esten. Suma los
    // bytes de indices a g_statUploadBytes (es LO que cruza el bus por frame).
    void DrawTrianglesClientIdx(int count, const MeshIndex* indices);
    // el gemelo en GL_LINES: aristas de un material con `lineas` sobre los
    // atributos que esten bindeados (VBO o client arrays; mismo contrato).
    void DrawLinesClientIdx(int count, const MeshIndex* indices);
    void UnbindVBOs();                 // vuelve a client-side arrays (bind ARRAY/ELEMENT a 0)

    // dibuja triangulos indexados (indices ushort)
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

    // wireframe (relleno por lineas). Solo GL de escritorio tiene glPolygonMode;
    // en GL ES es no-op (la malla sale rellena, como hoy en Symbian/Android).
    void Wireframe(bool on);

    // --- cache de estado ---
    // Enable/Disable, BindTexture Y el estado fino (depth func/mask, blend,
    // texenv, texgen/matcap, shade model, alpha test, client arrays) llevan
    // cache: cambiar a un estado que YA esta puesto no genera llamada GL. OJO:
    // el cache solo es correcto si TODO el estado pasa por el motor. Si algun
    // codigo todavia llama GL directo hay que llamar Invalidate() al volver al
    // motor para resincronizar. El pase de ESCENA se resincroniza UNA vez por
    // frame (antes del traversal), no por malla: nada de lo que corre adentro
    // del pase puede tocar GL crudo (lo vigila AuditarEstado en debug).
    void Invalidate();
    // auditoria del cache (PC/debug): compara el cache contra GL real via
    // glIsEnabled/glGet (flush en tilers: JAMAS por frame en dispositivo).
    // Devuelve la cantidad de desincronizaciones (0 = coherente) y las loguea.
    int  AuditarEstado();
    // el harness la prende ('glaudit'): el pase de escena audita al cerrar cada
    // frame y acumula los desyncs aca. Default off (cero costo).
    extern bool g_auditarEscena;
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
    extern int g_statUploadBytes;  // bytes subidos a buffer objects (VBO/IBO) este frame
    extern int g_statTrisOpacos;   // triangulos dibujados con Blend APAGADO
    extern int g_statTrisBlend;    // triangulos dibujados con Blend PRENDIDO
    // desglose de drawcalls por CATEGORIA: el pase que dibuja declara la suya
    // (StatsReset vuelve a Escena; el flush de particulas se marca solo).
    enum StatCat { StatCatEscena = 0, StatCatParticulas, StatCatUI, StatCatCount_ };
    extern int g_statDcCat[StatCatCount_];
    void StatCategoria(StatCat c);
    void StatsReset();
    void Finish();               // glFinish: drena el pipeline (medicion real de GPU en el bench)

    // Backend ES2/WebGL: la app lo llama UNA vez tras crear el contexto GL para cargar las
    // funciones GL2.0 (getProc = SDL_GL_GetProcAddress) y compilar el shader. Solo lo define
    // el backend gles2 (w3dGraphicsGLES2.cpp); el backend de pipeline fijo no lo usa.
    void GLES2Init(void* (*getProc)(const char*));
    // true SOLO durante el render FINAL (la foto/los pases): los objetos con
    // renderizable=false se saltean (en el viewport se ven igual)
    extern bool w3dRenderFinal;

    // PASE DE ESPEJO: true mientras el Mirror dibuja el reflejo. El material de
    // la malla NO puede decidir el z-test durante ese pase (el espejo ya lo
    // fijo). Sin esto, Mesh::AplicarMaterial hacia Enable(DepthTest) desde
    // mat->depth_test y DESHACIA el 'sinProfundidad' del espejo: el reflejo,
    // que queda mas hondo que el fondo opaco del estanque, no se veia nunca.
    extern bool w3dPaseEspejoSinZ;
}

// Estado de render (lo setea la app/editor antes de dibujar la escena; el Core lo LEE para
// dibujar segun el modo). El editor lo deriva de su RenderType/view: el Core NO conoce esos modos.
extern bool w3dRenderWireframe; // dibuja wireframe en vez del relleno
// wireframe SOLO de lo que se dibuja (default true): con visibilidad por triangulo
// activa (Mesh::pvsFaces, modificador Culling) las lineas se arman de la LISTA
// ACTIVA y no de todas las aristas. En false, el wireframe clasico (todo).
extern bool w3dRenderWireframeSoloVisible;
extern bool w3dRenderSolido;    // material por defecto sin texturas (modo Solid)
extern bool w3dRenderSinLuz;    // sin iluminacion, solo profundidad (modo ZBuffer)
extern bool w3dRenderLuces;     // aplicar las luces de escena (modo Rendered)
extern bool w3dRenderNormalColor; // dibuja la malla unlit con color = normal (debug de normales; modo Normal View)
extern bool w3dRenderAlpha;       // pase ALPHA (matte): blanco unlit + solo el alpha de la textura (sin fog)
extern bool w3dRenderOverlays;    // el editor quiere overlays (contorno de seleccion / overlay de edit); el Core lo LEE
extern bool w3dVerSeleccion;      // "Ver seleccion" (menu Select del viewport): OFF = sin contorno NI tinte de seleccion
extern bool g_xray;               // X-Ray (overlay): la malla EN EDICION se dibuja semitransparente sin z-test (retopo)
