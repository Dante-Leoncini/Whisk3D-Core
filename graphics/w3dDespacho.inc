// GENERADO por tools/generar_despacho.py: no editar a mano. Lo incluye graphics/w3dGraphicsDespacho.cpp.
void Enable(Cap c) { W3D_TABLA()->Enable(c); }
void Disable(Cap c) { W3D_TABLA()->Disable(c); }
void DepthFunc(DepthCmp c) { W3D_TABLA()->DepthFunc(c); }
void EnableArray(VArray a) { W3D_TABLA()->EnableArray(a); }
void DisableArray(VArray a) { W3D_TABLA()->DisableArray(a); }
int ArrayEstado(VArray a) { return W3D_TABLA()->ArrayEstado(a); }
void ClearColor(float r, float g, float b, float a) { W3D_TABLA()->ClearColor(r, g, b, a); }
void Clear(int bits) { W3D_TABLA()->Clear(bits); }
int PlanosRecorteMax() { return W3D_TABLA()->PlanosRecorteMax(); }
void PlanosRecorte(int n, const float* eq4) { W3D_TABLA()->PlanosRecorte(n, eq4); }
int EstencilBits() { return W3D_TABLA()->EstencilBits(); }
void EstencilLimpiar(int valor) { W3D_TABLA()->EstencilLimpiar(valor); }
void EstencilMarcar(int valor) { W3D_TABLA()->EstencilMarcar(valor); }
void EstencilDentro(int valor) { W3D_TABLA()->EstencilDentro(valor); }
void EstencilApagar() { W3D_TABLA()->EstencilApagar(); }
void Viewport(int x, int y, int w, int h) { W3D_TABLA()->Viewport(x, y, w, h); }
void Scissor(int x, int y, int w, int h) { W3D_TABLA()->Scissor(x, y, w, h); }
void GetViewport(int xywh[4]) { W3D_TABLA()->GetViewport(xywh); }
void GetScissor(int xywh[4]) { W3D_TABLA()->GetScissor(xywh); }
bool ScissorActivo() { return W3D_TABLA()->ScissorActivo(); }
void MatrixMode(Matrix m) { W3D_TABLA()->MatrixMode(m); }
void LoadIdentity() { W3D_TABLA()->LoadIdentity(); }
void PushMatrix() { W3D_TABLA()->PushMatrix(); }
void PopMatrix() { W3D_TABLA()->PopMatrix(); }
void Translatef(float x, float y, float z) { W3D_TABLA()->Translatef(x, y, z); }
void Rotatef(float angleDeg, float x, float y, float z) { W3D_TABLA()->Rotatef(angleDeg, x, y, z); }
void Scalef(float x, float y, float z) { W3D_TABLA()->Scalef(x, y, z); }
void MultMatrix(const float* m16) { W3D_TABLA()->MultMatrix(m16); }
void GetMatrix(Matrix m, float* out16) { W3D_TABLA()->GetMatrix(m, out16); }
void LoadMatrix(const float* m16) { W3D_TABLA()->LoadMatrix(m16); }
void MatricesSincronizar() { W3D_TABLA()->MatricesSincronizar(); }
void SesgoMetros(float m) { W3D_TABLA()->SesgoMetros(m); }
void Ortho(float l, float r, float b, float t, float n, float f) { W3D_TABLA()->Ortho(l, r, b, t, n, f); }
void Frustum(float l, float r, float b, float t, float n, float f) { W3D_TABLA()->Frustum(l, r, b, t, n, f); }
void Perspective(float fovYDeg, float aspect, float n, float f) { W3D_TABLA()->Perspective(fovYDeg, aspect, n, f); }
void DepthMask(bool escribir) { W3D_TABLA()->DepthMask(escribir); }
void LineWidth(float px) { W3D_TABLA()->LineWidth(px); }
void SmoothShading(bool suave) { W3D_TABLA()->SmoothShading(suave); }
void FastPerspective() { W3D_TABLA()->FastPerspective(); }
void FrontFace(bool ccw) { W3D_TABLA()->FrontFace(ccw); }
void CullCarasFrontales(bool on) { W3D_TABLA()->CullCarasFrontales(on); }
void ColorMask(bool r, bool g, bool b, bool a) { W3D_TABLA()->ColorMask(r, g, b, a); }
void PointSpriteCoordReplace(bool on) { W3D_TABLA()->PointSpriteCoordReplace(on); }
void BindTexture(unsigned int id) { W3D_TABLA()->BindTexture(id); }
void TexGenSphere(bool on) { W3D_TABLA()->TexGenSphere(on); }
void TexMatrixMatcap(bool on) { W3D_TABLA()->TexMatrixMatcap(on); }
void TexMatrixUV(float du, float dv, float su, float sv) { W3D_TABLA()->TexMatrixUV(du, dv, su, sv); }
bool TieneTexGen() { return W3D_TABLA()->TieneTexGen(); }
void TexEnvReplace(bool on) { W3D_TABLA()->TexEnvReplace(on); }
void TexEnvDot3(bool on) { W3D_TABLA()->TexEnvDot3(on); }
void SombreadoPorPixel(bool on) { W3D_TABLA()->SombreadoPorPixel(on); }
bool SombreadoPorPixelSoportado() { return W3D_TABLA()->SombreadoPorPixelSoportado(); }
void TexEnvAlphaOnly(bool on) { W3D_TABLA()->TexEnvAlphaOnly(on); }
bool PasesConShader() { return W3D_TABLA()->PasesConShader(); }
void PaseEspecial(int modo, const float* p) { W3D_TABLA()->PaseEspecial(modo, p); }
void AnteriorPointer3f(const float* p) { W3D_TABLA()->AnteriorPointer3f(p); }
bool PostEfecto(int efecto, unsigned int texImg, unsigned int texDat, int texW, int texH, const float* uv4, const float* p8) { return W3D_TABLA()->PostEfecto(efecto, texImg, texDat, texW, texH, uv4, p8); }
bool ShadersEscritos() { return W3D_TABLA()->ShadersEscritos(); }
int ShaderEscritoCompilar(const char* vertice, const char* fragmento, char* log, int logMax) { return W3D_TABLA()->ShaderEscritoCompilar(vertice, fragmento, log, logMax); }
void ShaderEscritoBorrar(int id) { W3D_TABLA()->ShaderEscritoBorrar(id); }
void ShaderEscritoUsar(int id, const float* p) { W3D_TABLA()->ShaderEscritoUsar(id, p); }
bool MezclaTexturasSoportada() { return W3D_TABLA()->MezclaTexturasSoportada(); }
void MezclaTexturas(unsigned int idB, float k, bool filtrado, bool repetir, int modo) { W3D_TABLA()->MezclaTexturas(idB, k, filtrado, repetir, modo); }
void MezclaTexturasOff() { W3D_TABLA()->MezclaTexturasOff(); }
bool MezclaTexturasActiva() { return W3D_TABLA()->MezclaTexturasActiva(); }
void MezclaTexturasUV(const float* p) { W3D_TABLA()->MezclaTexturasUV(p); }
void MezclaTexturasUV2s(const short* p) { W3D_TABLA()->MezclaTexturasUV2s(p); }
void MezclaTexturasUVVBO(unsigned int h) { W3D_TABLA()->MezclaTexturasUVVBO(h); }
void MezclaTexturasMatrizUV(float du, float dv, float su, float sv) { W3D_TABLA()->MezclaTexturasMatrizUV(du, dv, su, sv); }
void MezclaTexturaBorrada(unsigned int id) { W3D_TABLA()->MezclaTexturaBorrada(id); }
unsigned int ErrorGL() { return W3D_TABLA()->ErrorGL(); }
void Color4f(float r, float g, float b, float a) { W3D_TABLA()->Color4f(r, g, b, a); }
void Color4fv(const float* rgba) { W3D_TABLA()->Color4fv(rgba); }
void Color4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { W3D_TABLA()->Color4ub(r, g, b, a); }
void Material(MatParam p, const float* rgba) { W3D_TABLA()->Material(p, rgba); }
void MaterialShininess(float s) { W3D_TABLA()->MaterialShininess(s); }
void FogMode(bool linear) { W3D_TABLA()->FogMode(linear); }
void FogStart(float z) { W3D_TABLA()->FogStart(z); }
void FogEnd(float z) { W3D_TABLA()->FogEnd(z); }
void FogColor(const float* rgba) { W3D_TABLA()->FogColor(rgba); }
void FogModo(int modo) { W3D_TABLA()->FogModo(modo); }
void FogDensidad(float d) { W3D_TABLA()->FogDensidad(d); }
void Light0fv(LightFv p, const float* v) { W3D_TABLA()->Light0fv(p, v); }
void Light0f(LightF p, float v) { W3D_TABLA()->Light0f(p, v); }
void SetLightEnabled(unsigned int glLightId, bool on) { W3D_TABLA()->SetLightEnabled(glLightId, on); }
void LightNfv(int i, LightFv p, const float* v) { W3D_TABLA()->LightNfv(i, p, v); }
void LightNf(int i, LightF p, float v) { W3D_TABLA()->LightNf(i, p, v); }
void LightNSpot(int i, float corte, float exponente, const float* dir3) { W3D_TABLA()->LightNSpot(i, corte, exponente, dir3); }
void LightN(int i, bool on) { W3D_TABLA()->LightN(i, on); }
void LightModelAmbient(const float* rgba) { W3D_TABLA()->LightModelAmbient(rgba); }
void LightModelTwoSide(bool on) { W3D_TABLA()->LightModelTwoSide(on); }
bool IsEnabled(Cap c) { return W3D_TABLA()->IsEnabled(c); }
void ReadPixelsBegin(int x, int y, int w, int h) { W3D_TABLA()->ReadPixelsBegin(x, y, w, h); }
void ReadPixelsRGBA(int x, int y, int w, int h, unsigned char* pixels) { W3D_TABLA()->ReadPixelsRGBA(x, y, w, h, pixels); }
void BlendAlpha() { W3D_TABLA()->BlendAlpha(); }
void BlendMode(int modo) { W3D_TABLA()->BlendMode(modo); }
void BlurY(float amt) { W3D_TABLA()->BlurY(amt); }
void AlphaTest(float ref) { W3D_TABLA()->AlphaTest(ref); }
void AlfaEmpaquetado(bool activado) { W3D_TABLA()->AlfaEmpaquetado(activado); }
void SetMezcla(int m) { W3D_TABLA()->SetMezcla(m); }
void TexFilter(bool linear) { W3D_TABLA()->TexFilter(linear); }
void TexWrap(bool repeat) { W3D_TABLA()->TexWrap(repeat); }
void TexBaseLevel(int nivel) { W3D_TABLA()->TexBaseLevel(nivel); }
void VertexPointer2f(int strideBytes, const float* p) { W3D_TABLA()->VertexPointer2f(strideBytes, p); }
void VertexPointer3f(int strideBytes, const float* p) { W3D_TABLA()->VertexPointer3f(strideBytes, p); }
void VertexPointer4f(int strideBytes, const float* p) { W3D_TABLA()->VertexPointer4f(strideBytes, p); }
void VertexPointer3s(int strideBytes, const short* p) { W3D_TABLA()->VertexPointer3s(strideBytes, p); }
void VertexPointer2s(int strideBytes, const short* p) { W3D_TABLA()->VertexPointer2s(strideBytes, p); }
void ColorPointer4ub(const unsigned char* p) { W3D_TABLA()->ColorPointer4ub(p); }
void ColorPointer4f(int strideBytes, const float* p) { W3D_TABLA()->ColorPointer4f(strideBytes, p); }
void NormalPointer3b(const signed char* p) { W3D_TABLA()->NormalPointer3b(p); }
void TexCoordPointer2f(int strideBytes, const float* p) { W3D_TABLA()->TexCoordPointer2f(strideBytes, p); }
void TexCoordPointer2s(int strideBytes, const short* p) { W3D_TABLA()->TexCoordPointer2s(strideBytes, p); }
void TexCoordPointer3b(const signed char* p, int count) { W3D_TABLA()->TexCoordPointer3b(p, count); }
bool VBOSoportado() { return W3D_TABLA()->VBOSoportado(); }
unsigned int GenBuffer() { return W3D_TABLA()->GenBuffer(); }
void DeleteBuffer(unsigned int h) { W3D_TABLA()->DeleteBuffer(h); }
void ArrayBufferData(unsigned int h, const void* data, int bytes) { W3D_TABLA()->ArrayBufferData(h, data, bytes); }
void IndexBufferData(unsigned int h, const void* data, int bytes) { W3D_TABLA()->IndexBufferData(h, data, bytes); }
long VBOMemoria() { return W3D_TABLA()->VBOMemoria(); }
void VertexVBO(unsigned int h) { W3D_TABLA()->VertexVBO(h); }
void NormalVBO(unsigned int h) { W3D_TABLA()->NormalVBO(h); }
void ColorVBO(unsigned int h) { W3D_TABLA()->ColorVBO(h); }
void TexCoordVBO(unsigned int h) { W3D_TABLA()->TexCoordVBO(h); }
void VertexVBO3s(unsigned int h) { W3D_TABLA()->VertexVBO3s(h); }
void TexCoordVBO2s(unsigned int h) { W3D_TABLA()->TexCoordVBO2s(h); }
void BindIndexVBO(unsigned int h) { W3D_TABLA()->BindIndexVBO(h); }
void DrawTrianglesVBO(int count, int firstIndex) { W3D_TABLA()->DrawTrianglesVBO(count, firstIndex); }
void DrawTrianglesClientIdx(int count, const MeshIndex* indices) { W3D_TABLA()->DrawTrianglesClientIdx(count, indices); }
void DrawLinesClientIdx(int count, const MeshIndex* indices) { W3D_TABLA()->DrawLinesClientIdx(count, indices); }
void UnbindVBOs() { W3D_TABLA()->UnbindVBOs(); }
void DrawElements(Primitiva p, int count, const MeshIndex* indices) { W3D_TABLA()->DrawElements(p, count, indices); }
void DrawElements16(Primitiva p, int count, const unsigned short* indices) { W3D_TABLA()->DrawElements16(p, count, indices); }
void DrawElementsVBO(Primitiva p, int count, int firstIndex) { W3D_TABLA()->DrawElementsVBO(p, count, firstIndex); }
void DrawTriangles(int count, const MeshIndex* indices) { W3D_TABLA()->DrawTriangles(count, indices); }
void DrawTrianglesArray(int vertexCount) { W3D_TABLA()->DrawTrianglesArray(vertexCount); }
void DrawTrianglesArrayFrom(int first, int count) { W3D_TABLA()->DrawTrianglesArrayFrom(first, count); }
void DrawTrianglesByte(int count, const unsigned char* indices) { W3D_TABLA()->DrawTrianglesByte(count, indices); }
void DrawLines(int vertexCount) { W3D_TABLA()->DrawLines(vertexCount); }
void DrawLineStrip(int vertexCount) { W3D_TABLA()->DrawLineStrip(vertexCount); }
void DrawLineStripIndexed(int count, const unsigned short* indices) { W3D_TABLA()->DrawLineStripIndexed(count, indices); }
void DrawPoints(int vertexCount) { W3D_TABLA()->DrawPoints(vertexCount); }
void PointSize(float px) { W3D_TABLA()->PointSize(px); }
void DrawLinesIndexed(int count, const unsigned short* indices) { W3D_TABLA()->DrawLinesIndexed(count, indices); }
void DepthRange(float n, float f) { W3D_TABLA()->DepthRange(n, f); }
void PolygonOffset(float factor, float units) { W3D_TABLA()->PolygonOffset(factor, units); }
void Wireframe(bool on) { W3D_TABLA()->Wireframe(on); }
void Invalidate() { W3D_TABLA()->Invalidate(); }
void EstadoBase() { W3D_TABLA()->EstadoBase(); }
bool EstadoFotoSoportada() { return W3D_TABLA()->EstadoFotoSoportada(); }
void EstadoFotoTomar() { W3D_TABLA()->EstadoFotoTomar(); }
void EstadoFotoReponer() { W3D_TABLA()->EstadoFotoReponer(); }
int AuditarEstado() { return W3D_TABLA()->AuditarEstado(); }
void StatCategoria(StatCat c) { W3D_TABLA()->StatCategoria(c); }
StatCat StatCategoriaActual() { return W3D_TABLA()->StatCategoriaActual(); }
void StatsReset() { W3D_TABLA()->StatsReset(); }
void StatRaw(int n) { W3D_TABLA()->StatRaw(n); }
void TrazaGL(const char* llamada) { W3D_TABLA()->TrazaGL(llamada); }
void TrazaGLVer() { W3D_TABLA()->TrazaGLVer(); }
void StatsCuadro() { W3D_TABLA()->StatsCuadro(); }
void Finish() { W3D_TABLA()->Finish(); }
void Flush() { W3D_TABLA()->Flush(); }
void DibujarPendiente() { W3D_TABLA()->DibujarPendiente(); }
bool CopiaPantallaDadaVuelta() { return W3D_TABLA()->CopiaPantallaDadaVuelta(); }
int CopiaPantallaFila(int y, int h) { return W3D_TABLA()->CopiaPantallaFila(y, h); }
