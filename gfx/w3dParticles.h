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
    float tintR, tintG, tintB;    // tinte global (1,1,1 = sin tinte)
    float offsetX, offsetY, offsetZ; // corrimiento global del campo al DIBUJAR (parallax / mover el emisor)
    int   blend;                  // modo de mezcla (enum Mezcla). Default: MezclaAdd
    int   maxParts;               // tope de particulas vivas
    unsigned texs[8]; int nTex;   // texturas (elige una al azar por particula)

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
        fadeIn=0.12f; fadeOut=0.30f; rate=0; tintR=1; tintG=1; tintB=1;
        offsetX=0; offsetY=0; offsetZ=0;
        blend=MezclaAdd; maxParts=200; nTex=0; acc=0; phase=0; rng=2463534242u;
    }

    void SetTexturas(const unsigned* t, int n) { if(n>8)n=8; nTex=n; for(int i=0;i<n;i++) texs[i]=t[i]; }

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
            p.vx*=damp; p.vy*=damp; p.vz*=damp;
            float sway = swayAmp>0 ? swayAmp*sinf(phase*swayFreq + p.swayPh) : 0.0f;
            p.x += (p.vx + sway)*dt; p.y += p.vy*dt; p.z += p.vz*dt;
            p.rot += p.spin*dt;
            ++i;
        }
    }

    // brillo por vida: aparece en 'fadeIn' (arranque), se mantiene, y se apaga en 'fadeOut' (final,
    // llega a 0 al morir). Asi puede quedar brillante casi toda la vida y recien apagarse al final.
    float FadeDe(const Particle& p) const {
        float t = 1.0f - p.life/p.lifeMax;        // 0=nace, 1=muere
        float fin  = (fadeIn>0.0f  && t < fadeIn)       ? t/fadeIn         : 1.0f;
        float fout = (fadeOut>0.0f && t > 1.0f-fadeOut) ? (1.0f-t)/fadeOut : 1.0f;
        float f = fin<fout ? fin : fout;
        return f<0.0f ? 0.0f : f;
    }

    // dibuja los billboards (plano XY). El caller ya armo projection/view + Viewport + Enable(Blend) no hace falta.
    void Draw() {
        if (parts.empty() || nTex<=0) return;
        Disable(DepthTest); Disable(Lighting); Disable(CullFace);
        Enable(Texture2D); Enable(Blend); SetMezcla(blend);
        EnableArray(VertexArray); EnableArray(TexCoordArray);
        static const float UV[12] = { 0,0, 1,0, 1,1,  0,0, 1,1, 0,1 };
        for (size_t i=0; i<parts.size(); ++i) {
            const Particle& p = parts[i];
            float b = p.a0 * FadeDe(p); if (b<=0.0f) continue;
            float lt = 1.0f - p.life/p.lifeMax;
            float s = p.size + (p.sizeEnd - p.size)*lt;          // tamanio interpolado por vida
            float px = p.x+offsetX, py = p.y+offsetY, pz = p.z+offsetZ; // corrimiento global (parallax)
            float c = cosf(p.rot)*s, sn = sinf(p.rot)*s;         // quad rotado, medio-lado = s
            float x0=px-c+sn, y0=py-sn-c;   float x1=px+c+sn, y1=py+sn-c;
            float x2=px+c-sn, y2=py+sn+c;   float x3=px-c-sn, y3=py-sn+c;
            float V[18] = { x0,y0,pz, x1,y1,pz, x2,y2,pz,  x0,y0,pz, x2,y2,pz, x3,y3,pz };
            VertexPointer3f(0,V); TexCoordPointer2f(0,UV);
            BindTexture(texs[p.tex]);
            Color4f(tintR*b, tintG*b, tintB*b, b);               // additiva: alpha->0 = desaparece
            DrawTrianglesArray(6);
        }
        DisableArray(TexCoordArray);
        Disable(Blend);
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
