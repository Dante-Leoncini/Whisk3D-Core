#pragma once
// ============================================================================
//  w3dParticles.h — sistema de PARTICULAS flexible del Whisk3DCore.
//  Header-only, C++03 / Symbian-safe. Reutilizable (fondo, humo, chispas, magia...).
//
//  Configurable: gravedad, viento, friccion, bamboleo (onirico), caja de nacimiento,
//  velocidad inicial, vida, tamanio (inicial->final), alpha/brillo azaroso, giro,
//  emision continua (rate) o manual, modo de mezcla (Mezcla) y hasta 8 texturas
//  (elige una al azar por particula).
//
//  Las particulas se dibujan como billboards en el plano XY. Usa Draw() si vos
//  armaste projection/view, o DrawOrtho() para el caso 2D (ortho x:[0,aspect] y:[0,1],
//  con y=0 abajo). El RNG es un LCG deterministico (sin Math.random -> portable).
// ============================================================================

#include "w3dGraphics.h"
#include <vector>
#include <math.h>

namespace w3dEngine {

struct Particle {
    float x, y, z;        // posicion
    float vx, vy, vz;     // velocidad
    float life, lifeMax;  // vida restante / total (s)
    float size, sizeEnd;  // tamanio inicial y final (interpola por vida)
    float rot, spin;      // rotacion (rad) y velocidad angular (rad/s)
    float a0;             // alpha/brillo base azaroso (0..1)
    float swayPh;         // fase del bamboleo (por particula)
    int   tex;            // indice en texs[]
};

struct ParticleSystem {
    // ---------- CONFIG (defaults tipo "burbujas que suben"; toca lo que quieras) ----------
    float gravX, gravY, gravZ;    // aceleracion constante. Burbujas: gravY>0 (suben)
    float windX, windY, windZ;    // viento (aceleracion extra)
    float damping;                // friccion (1/s). 0 = ninguna
    float swayAmp, swayFreq;      // bamboleo horizontal (onirico). swayAmp=0 lo apaga
    float spawnX0, spawnX1, spawnY0, spawnY1, spawnZ0, spawnZ1; // caja de nacimiento
    float velX0, velX1, velY0, velY1, velZ0, velZ1;            // velocidad inicial (rango azaroso)
    float lifeMin, lifeMax;       // duracion de vida (s)
    float sizeMin, sizeMax;       // tamanio inicial (azaroso)
    float sizeEndMul;             // tamanio al morir = inicial * sizeEndMul
    float alphaMin, alphaMax;     // alpha/brillo base azaroso (0..1)
    float spinMin, spinMax;       // giro azaroso (rad/s)
    float fadeIn;                 // fraccion de vida para APARECER (0..1)
    float fadeOut;                // fraccion de vida para APAGARSE al final (0..1); llega a 0 al morir
    float rate;                   // particulas por segundo (emision continua). 0 = solo manual (EmitN)
    // TURBULENCIA: deriva azarosa SUAVE por particula durante la vida (unidades/s^2).
    // No es ruido por frame (no vibra): cada particula lleva su propio "wander" que
    // cambia de direccion de a poco (senos con fases personales, deterministico via
    // swayPh). 0 = apagada, cero costo (default: los usuarios viejos no cambian).
    float turbAmp;
    float tintR, tintG, tintB;    // tinte global (1,1,1 = sin tinte)
    float offsetX, offsetY, offsetZ; // corrimiento global del campo al DIBUJAR (parallax / mover el emisor)
    int   blend;                  // modo de mezcla (enum Mezcla). Default: MezclaAdd
    // Con mezcla ADITIVA el color se multiplica por el alfa (asi la particula "desaparece").
    // Con mezcla ALFA eso OSCURECE la particula mientras se desvanece, que casi nunca es lo que
    // se quiere: colorPlano deja el color quieto y solo baja el alfa.
    bool  colorPlano;
    int   maxParts;               // tope de particulas vivas
    enum { MaxTexs = 8 };
    unsigned texs[MaxTexs]; int nTex;   // texturas (elige una al azar por particula)
    // FLIPBOOK por EDAD: la textura es un atlas de flipCols x flipFilas celdas; cada particula
    // recorre flipCuadros celdas segun su edad (0=nace -> flipCuadros-1=muere). flipCuadros=0 = sin
    // flipbook (UV entero). Es CONSTANTE (celda entera por edad): 0 mezcla, solo apunta a la celda.
    int flipCols, flipFilas, flipCuadros;
    // SUB-RECT del atlas unico (default 0,0,1,1 = la textura entera): el sprite
    // (y su grilla de flipbook) viven en [uvU0..uvU1] x [uvV0..uvV1]. Con TODOS
    // los emisores apuntando al mismo atlas, el pase de particulas queda en UN
    // solo bind de textura (los draws solo se parten por mezcla).
    float uvU0, uvV0, uvU1, uvV1;

    // ---------- RUNTIME ----------
    std::vector<Particle> parts;
    float acc;    // acumulador de emision
    float phase;  // tiempo total (para el bamboleo)
    unsigned rng; // semilla del RNG (LCG)

    ParticleSystem() { Defaults(); }

    void Defaults() {
        gravX=0; gravY=0; gravZ=0; windX=0; windY=0; windZ=0; damping=0;
        swayAmp=0; swayFreq=1.0f;
        spawnX0=0; spawnX1=1; spawnY0=0; spawnY1=0; spawnZ0=0; spawnZ1=0;
        velX0=0; velX1=0; velY0=0.10f; velY1=0.20f; velZ0=0; velZ1=0;
        lifeMin=6; lifeMax=12; sizeMin=0.03f; sizeMax=0.08f; sizeEndMul=1.0f;
        alphaMin=0.3f; alphaMax=1.0f; spinMin=-0.5f; spinMax=0.5f;
        fadeIn=0.12f; fadeOut=0.30f; rate=0; turbAmp=0; tintR=1; tintG=1; tintB=1;
        offsetX=0; offsetY=0; offsetZ=0;
        blend=MezclaAdd; colorPlano=false; maxParts=200; nTex=0; acc=0; phase=0; rng=2463534242u;
        flipCols=1; flipFilas=1; flipCuadros=0;
        uvU0=0; uvV0=0; uvU1=1; uvV1=1;
    }

    // UV (12 floats = 2 triangulos) del CUADRO del flipbook para una edad lt (0..1). Sin flipbook
    // (flipCuadros<=0) devuelve el quad entero. Celda entera por edad: constante, sin curvas ni CPU.
    // Todo REMAPEADO al sub-rect uv* (0,0,1,1 = identico a antes).
    void CeldaUV(float lt, float* out) const {
        float ru = uvU1 - uvU0, rv = uvV1 - uvV0;
        if (flipCuadros <= 0) {
            out[0]=uvU0; out[1]=uvV0;  out[2]=uvU1; out[3]=uvV0;  out[4]=uvU1;  out[5]=uvV1;
            out[6]=uvU0; out[7]=uvV0;  out[8]=uvU1; out[9]=uvV1;  out[10]=uvU0; out[11]=uvV1;
            return;
        }
        int cel = (int)(lt * flipCuadros); if (cel < 0) cel = 0; if (cel >= flipCuadros) cel = flipCuadros - 1;
        int cols = flipCols > 0 ? flipCols : 1, filas = flipFilas > 0 ? flipFilas : 1;
        int col = cel % cols, fila = (cel / cols) % filas;
        float du = ru / (float)cols, dv = rv / (float)filas;
        float u0 = uvU0 + col * du, u1 = u0 + du;
        float v0 = uvV0 + fila * dv, v1 = v0 + dv;   // fila 0 = arriba del atlas
        out[0]=u0; out[1]=v0;  out[2]=u1; out[3]=v0;  out[4]=u1;  out[5]=v1;
        out[6]=u0; out[7]=v0;  out[8]=u1; out[9]=v1;  out[10]=u0; out[11]=v1;
    }

    void SetTexturas(const unsigned* t, int n) { if(n>MaxTexs)n=MaxTexs; nTex=n; for(int i=0;i<n;i++) texs[i]=t[i]; }

    // RNG: LCG -> [0,1). Deterministico (misma secuencia siempre); no usa Math.random.
    float frnd() { rng = rng*1664525u + 1013904223u; return (float)((rng>>8)&0xFFFFFFu)/16777216.0f; }
    float rnd(float a, float b) { return a + (b-a)*frnd(); }

    Particle* Emit() {
        if ((int)parts.size() >= maxParts) return 0;
        Particle p;
        p.x=rnd(spawnX0,spawnX1); p.y=rnd(spawnY0,spawnY1); p.z=rnd(spawnZ0,spawnZ1);
        p.vx=rnd(velX0,velX1); p.vy=rnd(velY0,velY1); p.vz=rnd(velZ0,velZ1);
        p.lifeMax=rnd(lifeMin,lifeMax); if(p.lifeMax<0.01f)p.lifeMax=0.01f; p.life=p.lifeMax;
        p.size=rnd(sizeMin,sizeMax); p.sizeEnd=p.size*sizeEndMul;
        p.rot=rnd(0.0f,6.2831853f); p.spin=rnd(spinMin,spinMax);
        p.a0=rnd(alphaMin,alphaMax); p.swayPh=rnd(0.0f,6.2831853f);
        p.tex = nTex>0 ? (int)(frnd()*nTex) : 0; if(nTex>0 && p.tex>=nTex) p.tex=nTex-1;
        parts.push_back(p);
        return &parts.back(); // ojo: si emitis mas, el vector puede realojar; usalo al toque
    }
    void EmitN(int k) { for(int i=0;i<k;i++) if(!Emit()) break; }

    // emite UNA particula EN EL PUNTO (px,py,pz) con velocidad de MODULO 'vel' en una
    // direccion azarosa dentro del CONO de apertura 'aperturaDeg' (angulo TOTAL, en grados)
    // alrededor del eje (ax,ay,az). apertura 0 = chorro recto; 360 = esfera entera.
    // El resto (vida, tamanio, alpha, giro, textura) sale de la config como en Emit().
    // Lo usa el emisor 3D del editor (objeto Particulas): el cono es la "dispersion".
    Particle* EmitCono(float px, float py, float pz,
                       float ax, float ay, float az,
                       float vel, float aperturaDeg) {
        Particle* p = Emit();
        if (!p) return 0;
        p->x = px; p->y = py; p->z = pz;
        // eje normalizado (si es ~0, se emite hacia +Y)
        float al = sqrtf(ax*ax + ay*ay + az*az);
        if (al < 1e-6f) { ax = 0; ay = 1; az = 0; } else { ax/=al; ay/=al; az/=al; }
        // direccion azarosa uniforme en el CASQUETE de medio-angulo apertura/2:
        // cos(theta) uniforme en [cos(max), 1] y phi uniforme en [0, 2pi)
        float maxRad = aperturaDeg * 0.5f * 0.0174532925f;
        if (maxRad < 0.0f) maxRad = 0.0f; if (maxRad > 3.14159265f) maxRad = 3.14159265f;
        float cosMax = cosf(maxRad);
        float ct = cosMax + (1.0f - cosMax) * frnd();
        float st = sqrtf(1.0f - ct*ct);
        float phi = rnd(0.0f, 6.2831853f);
        // base ortonormal {u,v} perpendicular al eje (el eje menos alineado del mundo)
        float rx, ry, rz;
        if (ax*ax < 0.5f) { rx = 1; ry = 0; rz = 0; } else { rx = 0; ry = 1; rz = 0; }
        float ux = ry*az - rz*ay, uy = rz*ax - rx*az, uz = rx*ay - ry*ax;
        float ul = sqrtf(ux*ux + uy*uy + uz*uz); ux/=ul; uy/=ul; uz/=ul;
        float vx = ay*uz - az*uy, vy = az*ux - ax*uz, vz = ax*uy - ay*ux;
        float dx = ax*ct + (ux*cosf(phi) + vx*sinf(phi))*st;
        float dy = ay*ct + (uy*cosf(phi) + vy*sinf(phi))*st;
        float dz = az*ct + (uz*cosf(phi) + vz*sinf(phi))*st;
        p->vx = dx*vel; p->vy = dy*vel; p->vz = dz*vel;
        return p;
    }
    // "precalienta" el efecto: corre 'secs' segundos para que al arrancar ya haya particulas repartidas.
    void Prewarm(float secs, float step) { if(step<=0)step=0.05f; for(float t=0;t<secs;t+=step) Update(step); }

    void Update(float dt) {
        if (dt<=0) return; if (dt>0.1f) dt=0.1f; // dt acotado (pestania inactiva)
        phase += dt;
        if (rate>0) { acc += rate*dt; while(acc>=1.0f){ acc-=1.0f; if(!Emit()){ acc=0; break; } } }
        float damp = damping>0 ? (1.0f - damping*dt) : 1.0f; if(damp<0)damp=0;
        for (size_t i=0; i<parts.size(); ) {
            Particle& p = parts[i];
            p.life -= dt;
            if (p.life<=0) { parts[i]=parts.back(); parts.pop_back(); continue; } // swap-remove
            p.vx += (gravX+windX)*dt; p.vy += (gravY+windY)*dt; p.vz += (gravZ+windZ)*dt;
            if (turbAmp > 0.0f) {
                // wander SUAVE por particula: aceleracion que rota de a poco, con fases
                // personales (swayPh) -> cada particula deriva distinto, sin vibrar.
                // Es funcion del tiempo CONTINUO (phase), integrada con dt: no depende
                // del framerate. Deterministica (swayPh sale del LCG del sistema).
                float ph = p.swayPh;
                p.vx += turbAmp * sinf(phase*1.7f + ph*3.1f) * dt;
                p.vy += turbAmp * sinf(phase*1.3f + ph*5.7f) * dt;
                p.vz += turbAmp * cosf(phase*1.9f + ph*2.3f) * dt;
            }
            p.vx*=damp; p.vy*=damp; p.vz*=damp;
            float sway = swayAmp>0 ? swayAmp*sinf(phase*swayFreq + p.swayPh) : 0.0f;
            p.x += (p.vx + sway)*dt; p.y += p.vy*dt; p.z += p.vz*dt;
            p.rot += p.spin*dt;
            ++i;
        }
    }

    // brillo por vida: aparece en 'fadeIn' (arranque), se mantiene, y se apaga en 'fadeOut' (final,
    // llega a 0 al morir). Asi puede quedar brillante casi toda la vida y recien apagarse al final.
    float BrilloPorVida(const Particle& p) const {   // 0..1 segun cuanta vida le queda
        float t = 1.0f - p.life/p.lifeMax;        // 0=nace, 1=muere
        float fin  = (fadeIn>0.0f  && t < fadeIn)       ? t/fadeIn         : 1.0f;
        float fout = (fadeOut>0.0f && t > 1.0f-fadeOut) ? (1.0f-t)/fadeOut : 1.0f;
        float f = fin<fout ? fin : fout;
        return f<0.0f ? 0.0f : f;
    }

    // dibuja los billboards (plano XY). El caller ya armo projection/view + Viewport + Enable(Blend) no hace falta.
    // (contrato historico: ADEMAS apaga el depth test, como siempre; el caso 2D dibuja encima de todo)
    void Draw() {
        if (parts.empty() || nTex<=0) return;   // sin nada que dibujar NO se toca ningun estado (como siempre)
        Disable(DepthTest);
        DrawBillboard(1,0,0, 0,1,0);   // plano XY = right (1,0,0) y up (0,1,0): identico al Draw de siempre
    }

    // dibuja los billboards en el plano que arman los versores 'right'/'up' que pasa el
    // caller (en 3D: los ejes de la CAMARA -> cada quad mira a la camara). A diferencia
    // de Draw(), NO toca el depth test: el caller 3D lo deja PRENDIDO (las particulas se
    // tapan detras de los muros) con DepthMask(false) para no escribirlo entre ellas.
    // arma el ESTADO COMPLETO del dibujo de billboards, sin heredar NADA del que
    // dibujo antes (la raiz del "humo verde": tras una escena 3D el ColorArray del
    // ultimo mesh quedaba prendido, el Color4f de DrawBillboardUno se IGNORABA y el
    // color/alpha salian de un puntero de colores de vertice viejo -verde sobre el
    // pasto-; y un TexEnv distinto de MODULATE pisaba la textura). En el caso 2D
    // historico todo esto ya estaba apagado: apagarlo de nuevo no cambia nada alli.
    // NO toca el depth test (contrato de DrawBillboard) ni la mezcla (es por sistema,
    // la fija DrawBillboardUno). Es estatico: el pase GLOBAL del editor lo llama una
    // vez y despues intercala particulas de VARIOS sistemas ordenadas por profundidad.
    static void DrawBillboardEstado() {
        Disable(Lighting); Disable(CullFace); Disable(Fog);
        DisableArray(ColorArray); DisableArray(NormalArray);
        TexEnvReplace(false); TexEnvAlphaOnly(false); TexEnvDot3(false);   // MODULATE
        Enable(Texture2D); Enable(Blend);
        EnableArray(VertexArray); EnableArray(TexCoordArray);
    }
    static void DrawBillboardFin() {
        DisableArray(TexCoordArray);
        Disable(Blend);
    }

    // BATCHEO (P2): en vez de dibujar la particula YA (DrawBillboardUno = un draw
    // call POR particula), APPENDEA sus 6 vertices (pos + uv + color POR VERTICE)
    // a los buffers del lote del caller. El caller corta el lote cuando cambia la
    // corrida (textura / mezcla / tope) y dibuja la corrida ENTERA con UN draw
    // (ver el flush global del editor): 100 particulas pasan de 100 draws a 2-4.
    // Mismos numeros que DrawBillboardUno (tamanio por vida, giro, brillo, tinte);
    // el color uniforme de antes viaja ahora por vertice (mismo pixel final).
    // false = la particula no aporta (alpha 0): no appendea nada.
    bool AppendBillboard(const Particle& p,
                         float rx, float ry, float rz, float ux, float uy, float uz,
                         std::vector<float>& pos, std::vector<float>& uvs,
                         std::vector<unsigned char>& col) const {
        float b = p.a0 * BrilloPorVida(p); if (b <= 0.0f) return false;
        float lt = 1.0f - p.life/p.lifeMax;
        float UV[12]; CeldaUV(lt, UV);   // flipbook por edad (o quad entero si no hay flipbook)
        float s = p.size + (p.sizeEnd - p.size)*lt;
        float px = p.x+offsetX, py = p.y+offsetY, pz = p.z+offsetZ;
        float c = cosf(p.rot)*s, sn = sinf(p.rot)*s;
        float exx = rx*c + ux*sn, exy = ry*c + uy*sn, exz = rz*c + uz*sn;
        float eyx = ux*c - rx*sn, eyy = uy*c - ry*sn, eyz = uz*c - rz*sn;
        const float V[18] = {
            px-exx-eyx, py-exy-eyy, pz-exz-eyz,   px+exx-eyx, py+exy-eyy, pz+exz-eyz,
            px+exx+eyx, py+exy+eyy, pz+exz+eyz,   px-exx-eyx, py-exy-eyy, pz-exz-eyz,
            px+exx+eyx, py+exy+eyy, pz+exz+eyz,   px-exx+eyx, py-exy+eyy, pz-exz+eyz };
        float r, g, bl, a;
        if (colorPlano) { r = tintR;   g = tintG;   bl = tintB;   a = b; }
        else            { r = tintR*b; g = tintG*b; bl = tintB*b; a = b; }
        if (r < 0) r = 0; if (r > 1) r = 1; if (g < 0) g = 0; if (g > 1) g = 1;
        if (bl < 0) bl = 0; if (bl > 1) bl = 1; if (a < 0) a = 0; if (a > 1) a = 1;
        unsigned char cr = (unsigned char)(r*255.0f + 0.5f), cg = (unsigned char)(g*255.0f + 0.5f);
        unsigned char cb = (unsigned char)(bl*255.0f + 0.5f), ca = (unsigned char)(a*255.0f + 0.5f);
        for (int i = 0; i < 18; i++) pos.push_back(V[i]);
        for (int i = 0; i < 12; i++) uvs.push_back(UV[i]);
        for (int i = 0; i < 6; i++) { col.push_back(cr); col.push_back(cg); col.push_back(cb); col.push_back(ca); }
        return true;
    }

    // dibuja UNA particula de ESTE sistema como billboard en el plano (right, up).
    // Requiere el estado de DrawBillboardEstado ya armado; fija la mezcla y la
    // textura del sistema en cada llamada (baratas: BindTexture cachea) para poder
    // INTERCALAR particulas de sistemas distintos (el orden global por profundidad).
    void DrawBillboardUno(const Particle& p,
                          float rx, float ry, float rz, float ux, float uy, float uz) {
        if (nTex<=0) return;
        float b = p.a0 * BrilloPorVida(p); if (b<=0.0f) return;
        float lt = 1.0f - p.life/p.lifeMax;
        float UV[12]; CeldaUV(lt, UV);   // flipbook por edad (o quad entero si no hay flipbook)
        float s = p.size + (p.sizeEnd - p.size)*lt;          // tamanio interpolado por vida
        float px = p.x+offsetX, py = p.y+offsetY, pz = p.z+offsetZ; // corrimiento global (parallax)
        float c = cosf(p.rot)*s, sn = sinf(p.rot)*s;         // quad rotado, medio-lado = s
        // eje X del quad = right*cos + up*sin; eje Y = -right*sin + up*cos (giro DENTRO del plano)
        float exx = rx*c + ux*sn, exy = ry*c + uy*sn, exz = rz*c + uz*sn;
        float eyx = ux*c - rx*sn, eyy = uy*c - ry*sn, eyz = uz*c - rz*sn;
        float V[18] = {
            px-exx-eyx, py-exy-eyy, pz-exz-eyz,   px+exx-eyx, py+exy-eyy, pz+exz-eyz,
            px+exx+eyx, py+exy+eyy, pz+exz+eyz,   px-exx-eyx, py-exy-eyy, pz-exz-eyz,
            px+exx+eyx, py+exy+eyy, pz+exz+eyz,   px-exx+eyx, py-exy+eyy, pz-exz+eyz };
        SetMezcla(blend);
        VertexPointer3f(0,V); TexCoordPointer2f(0,UV);
        BindTexture(texs[p.tex]);
        if (colorPlano) Color4f(tintR, tintG, tintB, b);      // el color no se apaga: solo el alfa
        else            Color4f(tintR*b, tintG*b, tintB*b, b); // aditiva: alpha->0 = desaparece
        DrawTrianglesArray(6);
    }

    void DrawBillboard(float rx, float ry, float rz, float ux, float uy, float uz) {
        if (parts.empty() || nTex<=0) return;
        DrawBillboardEstado();
        for (size_t i=0; i<parts.size(); ++i)
            DrawBillboardUno(parts[i], rx, ry, rz, ux, uy, uz);
        DrawBillboardFin();
    }

    // caso 2D: arma un ortho x:[0,aspect] y:[0,1] (y=0 ABAJO) y dibuja. Las coords de config
    // (spawn, velocidad, tamanio) van en esas unidades (fraccion del ALTO). Poné spawnX1=aspect.
    void DrawOrtho(int vx, int vy, int vw, int vh) {
        if (vw<1 || vh<1) return;
        Viewport(vx,vy,vw,vh);
        float aspect = (float)vw/(float)vh;
        MatrixMode(Projection); LoadIdentity();
        Ortho(0.0f, aspect, 0.0f, 1.0f, -1.0f, 1.0f);
        MatrixMode(ModelView); LoadIdentity();
        Draw();
    }

    void Clear() { parts.clear(); acc=0; }
};

} // namespace w3dEngine
