// GENERADO por tools/generar_despacho.py: no editar a mano. Lo incluye graphics/w3dGraphicsDispatch.cpp.
void Enable(Cap c) { W3D_TABLE()->Enable(c); }
void Disable(Cap c) { W3D_TABLE()->Disable(c); }
void DepthFunc(DepthCmp c) { W3D_TABLE()->DepthFunc(c); }
void EnableArray(VArray a) { W3D_TABLE()->EnableArray(a); }
void DisableArray(VArray a) { W3D_TABLE()->DisableArray(a); }
int ArrayState(VArray a) { return W3D_TABLE()->ArrayState(a); }
void ClearColor(float r, float g, float b, float a) { W3D_TABLE()->ClearColor(r, g, b, a); }
void Clear(int bits) { W3D_TABLE()->Clear(bits); }
int PlanesClipMax() { return W3D_TABLE()->PlanesClipMax(); }
void PlanesClip(int n, const float* eq4) { W3D_TABLE()->PlanesClip(n, eq4); }
int StencilBits() { return W3D_TABLE()->StencilBits(); }
void StencilClear(int value) { W3D_TABLE()->StencilClear(value); }
void StencilMark(int value) { W3D_TABLE()->StencilMark(value); }
void StencilInside(int value) { W3D_TABLE()->StencilInside(value); }
void StencilTurnOff() { W3D_TABLE()->StencilTurnOff(); }
void Viewport(int x, int y, int w, int h) { W3D_TABLE()->Viewport(x, y, w, h); }
void Scissor(int x, int y, int w, int h) { W3D_TABLE()->Scissor(x, y, w, h); }
void GetViewport(int xywh[4]) { W3D_TABLE()->GetViewport(xywh); }
void GetScissor(int xywh[4]) { W3D_TABLE()->GetScissor(xywh); }
bool ScissorActive() { return W3D_TABLE()->ScissorActive(); }
void MatrixMode(Matrix m) { W3D_TABLE()->MatrixMode(m); }
void LoadIdentity() { W3D_TABLE()->LoadIdentity(); }
void PushMatrix() { W3D_TABLE()->PushMatrix(); }
void PopMatrix() { W3D_TABLE()->PopMatrix(); }
void Translatef(float x, float y, float z) { W3D_TABLE()->Translatef(x, y, z); }
void Rotatef(float angleDeg, float x, float y, float z) { W3D_TABLE()->Rotatef(angleDeg, x, y, z); }
void Scalef(float x, float y, float z) { W3D_TABLE()->Scalef(x, y, z); }
void MultMatrix(const float* m16) { W3D_TABLE()->MultMatrix(m16); }
void GetMatrix(Matrix m, float* out16) { W3D_TABLE()->GetMatrix(m, out16); }
void LoadMatrix(const float* m16) { W3D_TABLE()->LoadMatrix(m16); }
void MatricesSync() { W3D_TABLE()->MatricesSync(); }
void BiasMeters(float m) { W3D_TABLE()->BiasMeters(m); }
void Ortho(float l, float r, float b, float t, float n, float f) { W3D_TABLE()->Ortho(l, r, b, t, n, f); }
void Frustum(float l, float r, float b, float t, float n, float f) { W3D_TABLE()->Frustum(l, r, b, t, n, f); }
void Perspective(float fovYDeg, float aspect, float n, float f) { W3D_TABLE()->Perspective(fovYDeg, aspect, n, f); }
void DepthMask(bool write) { W3D_TABLE()->DepthMask(write); }
void LineWidth(float px) { W3D_TABLE()->LineWidth(px); }
void SmoothShading(bool smooth) { W3D_TABLE()->SmoothShading(smooth); }
void FastPerspective() { W3D_TABLE()->FastPerspective(); }
void FrontFace(bool ccw) { W3D_TABLE()->FrontFace(ccw); }
void CullFrontalFaces(bool on) { W3D_TABLE()->CullFrontalFaces(on); }
void ColorMask(bool r, bool g, bool b, bool a) { W3D_TABLE()->ColorMask(r, g, b, a); }
void PointSpriteCoordReplace(bool on) { W3D_TABLE()->PointSpriteCoordReplace(on); }
void BindTexture(unsigned int id) { W3D_TABLE()->BindTexture(id); }
void TexGenSphere(bool on) { W3D_TABLE()->TexGenSphere(on); }
void TexMatrixMatcap(bool on) { W3D_TABLE()->TexMatrixMatcap(on); }
void TexMatrixUV(float du, float dv, float su, float sv) { W3D_TABLE()->TexMatrixUV(du, dv, su, sv); }
bool HasTexGen() { return W3D_TABLE()->HasTexGen(); }
void TexEnvReplace(bool on) { W3D_TABLE()->TexEnvReplace(on); }
void TexEnvDot3(bool on) { W3D_TABLE()->TexEnvDot3(on); }
void ShadingPerPixel(bool on) { W3D_TABLE()->ShadingPerPixel(on); }
bool ShadingPerPixelSupported() { return W3D_TABLE()->ShadingPerPixelSupported(); }
void TexEnvAlphaOnly(bool on) { W3D_TABLE()->TexEnvAlphaOnly(on); }
bool PassesWithShader() { return W3D_TABLE()->PassesWithShader(); }
void SpecialPass(int mode, const float* p) { W3D_TABLE()->SpecialPass(mode, p); }
void PrevPointer3f(const float* p) { W3D_TABLE()->PrevPointer3f(p); }
bool PostEffect(int effect, unsigned int texImg, unsigned int texDat, int texW, int texH, const float* uv4, const float* p8) { return W3D_TABLE()->PostEffect(effect, texImg, texDat, texW, texH, uv4, p8); }
bool ShadersWritten() { return W3D_TABLE()->ShadersWritten(); }
int WrittenShaderCompile(const char* vertex, const char* fragment, char* log, int logMax) { return W3D_TABLE()->WrittenShaderCompile(vertex, fragment, log, logMax); }
void WrittenShaderDelete(int id) { W3D_TABLE()->WrittenShaderDelete(id); }
void WrittenShaderUse(int id, const float* p) { W3D_TABLE()->WrittenShaderUse(id, p); }
bool MixTexturesSupported() { return W3D_TABLE()->MixTexturesSupported(); }
void MixTextures(unsigned int idB, float k, bool filtered, bool repeat, int mode) { W3D_TABLE()->MixTextures(idB, k, filtered, repeat, mode); }
void MixTexturesOff() { W3D_TABLE()->MixTexturesOff(); }
bool MixTexturesActive() { return W3D_TABLE()->MixTexturesActive(); }
void MixTexturesUV(const float* p) { W3D_TABLE()->MixTexturesUV(p); }
void MixTexturesUV2s(const short* p) { W3D_TABLE()->MixTexturesUV2s(p); }
void MixTexturesUVVBO(unsigned int h) { W3D_TABLE()->MixTexturesUVVBO(h); }
void MixTexturesMatrixUV(float du, float dv, float su, float sv) { W3D_TABLE()->MixTexturesMatrixUV(du, dv, su, sv); }
void MixDeletedTexture(unsigned int id) { W3D_TABLE()->MixDeletedTexture(id); }
unsigned int ErrorGL() { return W3D_TABLE()->ErrorGL(); }
void Color4f(float r, float g, float b, float a) { W3D_TABLE()->Color4f(r, g, b, a); }
void Color4fv(const float* rgba) { W3D_TABLE()->Color4fv(rgba); }
void Color4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { W3D_TABLE()->Color4ub(r, g, b, a); }
void Material(MatParam p, const float* rgba) { W3D_TABLE()->Material(p, rgba); }
void MaterialShininess(float s) { W3D_TABLE()->MaterialShininess(s); }
void MaterialPBR(float metal, float rug, float f0, float trans, bool ssr) { W3D_TABLE()->MaterialPBR(metal, rug, f0, trans, ssr); }
bool MaterialMaps(unsigned n, unsigned r, unsigned m, unsigned a, unsigned e) { return W3D_TABLE()->MaterialMaps(n, r, m, a, e); }
void AmbientSH(const float* sh, const float* m) { W3D_TABLE()->AmbientSH(sh, m); }
bool ShadowsSupported() { return W3D_TABLE()->ShadowsSupported(); }
bool ShadowMapBegin(int w, int h) { return W3D_TABLE()->ShadowMapBegin(w, h); }
void ShadowMapBox(int x, int y, int w, int h) { W3D_TABLE()->ShadowMapBox(x, y, w, h); }
void ShadowMapFinish() { W3D_TABLE()->ShadowMapFinish(); }
void ShadowReceptors(const float* d) { W3D_TABLE()->ShadowReceptors(d); }
void ShadowReceive(int mode) { W3D_TABLE()->ShadowReceive(mode); }
bool CaptureBegin(int w, int h) { return W3D_TABLE()->CaptureBegin(w, h); }
void CaptureFinish() { W3D_TABLE()->CaptureFinish(); }
void Environment(unsigned int tex, unsigned int blurryTex, const float* m) { W3D_TABLE()->Environment(tex, blurryTex, m); }
void SceneCopy(bool on) { W3D_TABLE()->SceneCopy(on); }
bool GpuLimited() { return W3D_TABLE()->GpuLimited(); }
unsigned int UploadCube(const unsigned char* const faces[6], int side) { return W3D_TABLE()->UploadCube(faces, side); }
void EnvironmentCube(unsigned int cube, unsigned int blurryCube, const float* m) { W3D_TABLE()->EnvironmentCube(cube, blurryCube, m); }
void FogStart(float z) { W3D_TABLE()->FogStart(z); }
void FogEnd(float z) { W3D_TABLE()->FogEnd(z); }
void FogColor(const float* rgba) { W3D_TABLE()->FogColor(rgba); }
void FogMode(int mode) { W3D_TABLE()->FogMode(mode); }
void FogDensity(float d) { W3D_TABLE()->FogDensity(d); }
void Light0fv(LightFv p, const float* v) { W3D_TABLE()->Light0fv(p, v); }
void Light0f(LightF p, float v) { W3D_TABLE()->Light0f(p, v); }
void SetLightEnabled(unsigned int glLightId, bool on) { W3D_TABLE()->SetLightEnabled(glLightId, on); }
void LightNfv(int i, LightFv p, const float* v) { W3D_TABLE()->LightNfv(i, p, v); }
void LightNf(int i, LightF p, float v) { W3D_TABLE()->LightNf(i, p, v); }
void LightNSpot(int i, float cut, float exponent, const float* dir3) { W3D_TABLE()->LightNSpot(i, cut, exponent, dir3); }
void LightN(int i, bool on) { W3D_TABLE()->LightN(i, on); }
void LightModelAmbient(const float* rgba) { W3D_TABLE()->LightModelAmbient(rgba); }
void LightModelTwoSide(bool on) { W3D_TABLE()->LightModelTwoSide(on); }
bool IsEnabled(Cap c) { return W3D_TABLE()->IsEnabled(c); }
void ReadPixelsBegin(int x, int y, int w, int h) { W3D_TABLE()->ReadPixelsBegin(x, y, w, h); }
void ReadPixelsRGBA(int x, int y, int w, int h, unsigned char* pixels) { W3D_TABLE()->ReadPixelsRGBA(x, y, w, h, pixels); }
void BlendAlpha() { W3D_TABLE()->BlendAlpha(); }
void BlendMode(int mode) { W3D_TABLE()->BlendMode(mode); }
void BlurY(float amt) { W3D_TABLE()->BlurY(amt); }
void AlphaTest(float ref) { W3D_TABLE()->AlphaTest(ref); }
void AlphaPacked(bool activated) { W3D_TABLE()->AlphaPacked(activated); }
void SetMix(int m) { W3D_TABLE()->SetMix(m); }
void TexFilter(bool linear) { W3D_TABLE()->TexFilter(linear); }
void TexWrap(bool repeat) { W3D_TABLE()->TexWrap(repeat); }
void TexBaseLevel(int level) { W3D_TABLE()->TexBaseLevel(level); }
void VertexPointer2f(int strideBytes, const float* p) { W3D_TABLE()->VertexPointer2f(strideBytes, p); }
void VertexPointer3f(int strideBytes, const float* p) { W3D_TABLE()->VertexPointer3f(strideBytes, p); }
void VertexPointer4f(int strideBytes, const float* p) { W3D_TABLE()->VertexPointer4f(strideBytes, p); }
void VertexPointer3s(int strideBytes, const short* p) { W3D_TABLE()->VertexPointer3s(strideBytes, p); }
void VertexPointer2s(int strideBytes, const short* p) { W3D_TABLE()->VertexPointer2s(strideBytes, p); }
void ColorPointer4ub(const unsigned char* p) { W3D_TABLE()->ColorPointer4ub(p); }
void ColorPointer4f(int strideBytes, const float* p) { W3D_TABLE()->ColorPointer4f(strideBytes, p); }
void NormalPointer3b(const signed char* p) { W3D_TABLE()->NormalPointer3b(p); }
void TexCoordPointer2f(int strideBytes, const float* p) { W3D_TABLE()->TexCoordPointer2f(strideBytes, p); }
void TexCoordPointer2s(int strideBytes, const short* p) { W3D_TABLE()->TexCoordPointer2s(strideBytes, p); }
void TexCoordPointer3b(const signed char* p, int count) { W3D_TABLE()->TexCoordPointer3b(p, count); }
bool VBOSupported() { return W3D_TABLE()->VBOSupported(); }
unsigned int GenBuffer() { return W3D_TABLE()->GenBuffer(); }
void DeleteBuffer(unsigned int h) { W3D_TABLE()->DeleteBuffer(h); }
void ArrayBufferData(unsigned int h, const void* data, int bytes) { W3D_TABLE()->ArrayBufferData(h, data, bytes); }
void IndexBufferData(unsigned int h, const void* data, int bytes) { W3D_TABLE()->IndexBufferData(h, data, bytes); }
long VBOMemory() { return W3D_TABLE()->VBOMemory(); }
void VertexVBO(unsigned int h) { W3D_TABLE()->VertexVBO(h); }
void NormalVBO(unsigned int h) { W3D_TABLE()->NormalVBO(h); }
void ColorVBO(unsigned int h) { W3D_TABLE()->ColorVBO(h); }
void TexCoordVBO(unsigned int h) { W3D_TABLE()->TexCoordVBO(h); }
void VertexVBO3s(unsigned int h) { W3D_TABLE()->VertexVBO3s(h); }
void TexCoordVBO2s(unsigned int h) { W3D_TABLE()->TexCoordVBO2s(h); }
void BindIndexVBO(unsigned int h) { W3D_TABLE()->BindIndexVBO(h); }
void DrawTrianglesVBO(int count, int firstIndex) { W3D_TABLE()->DrawTrianglesVBO(count, firstIndex); }
void DrawTrianglesClientIdx(int count, const MeshIndex* indices) { W3D_TABLE()->DrawTrianglesClientIdx(count, indices); }
void DrawLinesClientIdx(int count, const MeshIndex* indices) { W3D_TABLE()->DrawLinesClientIdx(count, indices); }
void UnbindVBOs() { W3D_TABLE()->UnbindVBOs(); }
void DrawElements(Primitive p, int count, const MeshIndex* indices) { W3D_TABLE()->DrawElements(p, count, indices); }
void DrawElements16(Primitive p, int count, const unsigned short* indices) { W3D_TABLE()->DrawElements16(p, count, indices); }
void DrawElementsVBO(Primitive p, int count, int firstIndex) { W3D_TABLE()->DrawElementsVBO(p, count, firstIndex); }
void DrawTriangles(int count, const MeshIndex* indices) { W3D_TABLE()->DrawTriangles(count, indices); }
void DrawTrianglesArray(int vertexCount) { W3D_TABLE()->DrawTrianglesArray(vertexCount); }
void DrawTrianglesArrayFrom(int first, int count) { W3D_TABLE()->DrawTrianglesArrayFrom(first, count); }
void DrawTrianglesByte(int count, const unsigned char* indices) { W3D_TABLE()->DrawTrianglesByte(count, indices); }
void DrawLines(int vertexCount) { W3D_TABLE()->DrawLines(vertexCount); }
void DrawLineStrip(int vertexCount) { W3D_TABLE()->DrawLineStrip(vertexCount); }
void DrawLineStripIndexed(int count, const unsigned short* indices) { W3D_TABLE()->DrawLineStripIndexed(count, indices); }
void DrawPoints(int vertexCount) { W3D_TABLE()->DrawPoints(vertexCount); }
void PointSize(float px) { W3D_TABLE()->PointSize(px); }
void DrawLinesIndexed(int count, const unsigned short* indices) { W3D_TABLE()->DrawLinesIndexed(count, indices); }
void DepthRange(float n, float f) { W3D_TABLE()->DepthRange(n, f); }
void PolygonOffset(float factor, float units) { W3D_TABLE()->PolygonOffset(factor, units); }
void Wireframe(bool on) { W3D_TABLE()->Wireframe(on); }
void Invalidate() { W3D_TABLE()->Invalidate(); }
void BaseState() { W3D_TABLE()->BaseState(); }
bool SnapshotStateSupported() { return W3D_TABLE()->SnapshotStateSupported(); }
void SnapshotStateTake() { W3D_TABLE()->SnapshotStateTake(); }
void SnapshotStateRestore() { W3D_TABLE()->SnapshotStateRestore(); }
int AuditState() { return W3D_TABLE()->AuditState(); }
void StatCategory(StatCat c) { W3D_TABLE()->StatCategory(c); }
StatCat StatCurrentCategory() { return W3D_TABLE()->StatCurrentCategory(); }
void StatsReset() { W3D_TABLE()->StatsReset(); }
void StatRaw(int n) { W3D_TABLE()->StatRaw(n); }
void TraceGL(const char* call) { W3D_TABLE()->TraceGL(call); }
void TraceGLVer() { W3D_TABLE()->TraceGLVer(); }
void StatsFrame() { W3D_TABLE()->StatsFrame(); }
void Finish() { W3D_TABLE()->Finish(); }
void Flush() { W3D_TABLE()->Flush(); }
void DrawPending() { W3D_TABLE()->DrawPending(); }
bool CopyScreenFlipped() { return W3D_TABLE()->CopyScreenFlipped(); }
int CopyScreenRow(int y, int h) { return W3D_TABLE()->CopyScreenRow(y, h); }
