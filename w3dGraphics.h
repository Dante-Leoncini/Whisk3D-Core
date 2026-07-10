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
        Multisample     // anti-aliasing MSAA (hay que APAGARLO en el color-ID pick)
    };
    void Enable(Cap c);
    void Disable(Cap c);

    // comparacion del depth test
    enum DepthCmp { DepthLess, DepthLEqual, DepthEqual }; // DepthEqual: pasadas extra sobre la MISMA superficie
    void DepthFunc(DepthCmp c);

    // --- arrays de vertices del pipeline fijo ---
    enum VArray { VertexArray, TexCoordArray, NormalArray, ColorArray };
    void EnableArray(VArray a);
    void DisableArray(VArray a);

    // --- buffers ---
    void ClearColor(float r, float g, float b, float a);
    enum { ColorBuffer = 1, DepthBuffer = 2 };
    void Clear(int bits); // ColorBuffer | DepthBuffer

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
    unsigned int BoundTexture();       // la textura que el motor cree bindeada

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

    // --- parametros de la textura 2D ACTIVA (la del ultimo BindTexture) ---
    void TexFilter(bool linear); // true=LINEAR, false=NEAREST (min y mag)
    void TexWrap(bool repeat);   // true=REPEAT, false=CLAMP_TO_EDGE (S y T)

    // --- punteros de los arrays del pipeline fijo (stride en BYTES) ---
    void VertexPointer2f(int strideBytes, const float* p);         // 2 float (UI 2D)
    void VertexPointer3f(int strideBytes, const float* p);         // 3 float
    void VertexPointer3s(int strideBytes, const short* p);         // 3 short (verts enteros)
    void VertexPointer2s(int strideBytes, const short* p);         // 2 short (UI 2D entera)
    void ColorPointer4ub(const unsigned char* p);                  // 4 ubyte
    void NormalPointer3b(const signed char* p);                    // 3 byte
    void TexCoordPointer2f(int strideBytes, const float* p);       // 2 float
    void TexCoordPointer3b(const signed char* p);                  // 3 byte (normales como texcoords, para el matcap HW)

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
    // Enable/Disable y BindTexture llevan un cache: cambiar a un estado que YA
    // esta puesto no genera llamada GL. OJO: el cache solo es correcto si TODO
    // el estado pasa por el motor. Si algun codigo todavia llama GL directo
    // hay que llamar Invalidate() al volver al motor para
    // resincronizar el cache con el estado real de GL.
    void Invalidate();

    // Backend ES2/WebGL: la app lo llama UNA vez tras crear el contexto GL para cargar las
    // funciones GL2.0 (getProc = SDL_GL_GetProcAddress) y compilar el shader. Solo lo define
    // el backend gles2 (w3dGraphicsGLES2.cpp); el backend de pipeline fijo no lo usa.
    void GLES2Init(void* (*getProc)(const char*));
}

// Estado de render (lo setea la app/editor antes de dibujar la escena; el Core lo LEE para
// dibujar segun el modo). El editor lo deriva de su RenderType/view: el Core NO conoce esos modos.
extern bool w3dRenderWireframe; // dibuja wireframe en vez del relleno
extern bool w3dRenderSolido;    // material por defecto sin texturas (modo Solid)
extern bool w3dRenderSinLuz;    // sin iluminacion, solo profundidad (modo ZBuffer)
extern bool w3dRenderLuces;     // aplicar las luces de escena (modo Rendered)
extern bool w3dRenderNormalColor; // dibuja la malla unlit con color = normal (debug de normales; modo Normal View)
extern bool w3dRenderAlpha;       // pase ALPHA (matte): blanco unlit + solo el alpha de la textura (sin fog)
extern bool w3dRenderOverlays;    // el editor quiere overlays (contorno de seleccion / overlay de edit); el Core lo LEE
extern bool g_xray;               // X-Ray (overlay): la malla EN EDICION se dibuja semitransparente sin z-test (retopo)
