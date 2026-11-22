#ifndef FLIPBOOK_H
#define FLIPBOOK_H
#include <string>
#include <vector>
#include "animation/Animation.h"   // AnimProperty / keyFrame / EvalF (el motor de curvas se REUSA)

// ===========================================================================
//  FLIPBOOK — animacion de la UV de un QUAD (4 esquinas), como ASSET REUSABLE.
//
//  UNIFICA los dos flipbooks que habia sueltos (Mesh::UVAnimTira y Image2D.flip*)
//  en un solo concepto del Core. Lo consumen por NOMBRE:
//    * Image2D (HUD 2D): las 4 esquinas SON las UV de su quad.
//    * Mesh (AnimacionUV): las 4 esquinas mapean (bilineal) las UV de la malla.
//    * Particulas: idem billboard.
//
//  Las 8 curvas (esquina 0..3 x u,v) reusan AnimProperty -> constante/lineal/bezier
//  con tangentes, dopesheet, velocidad, sin matematica nueva.
//    * keyframes CONSTANTES  = flipbook clasico (la UV salta de celda): 0 mezcla, se
//      apunta a la UV que ya esta en memoria.
//    * keyframes BEZIER/LINEAL = la ventana UV se desliza/escala/rota (barato: sigue
//      siendo SOLO mover UV, sin tocar la textura).
//    * `crossfade` (opt-in) = mezclar dos celdas en un buffer CPU aparte; solo donde
//      se pide (por defecto se redondea a la celda mas cercana).
//
//  El ASSET no guarda estado de reproduccion: eso vive en FlipbookPlayer (uno por
//  consumidor), asi dos objetos pueden usar el MISMO flipbook con su propio desfase.
// ===========================================================================
class Flipbook {
public:
    std::string name;     // id del asset (lo referencian los consumidores)
    std::string atlas;    // ruta de la textura-atlas
    std::string lib;      // la LIBRERIA externa de la que es ("" = del proyecto). Uno de una libreria vive en la
                             // lista de la escena con su prefijo ("Efectos/Chispa") pero NUNCA se guarda en el
                             // proyecto: lo registra la libreria cada vez que se usa (main/io/Libraries.h)
    int   cols, rows;       // grilla del atlas (autoria por celdas)
    int   frames;           // largo del ciclo en cuadros logicos
    float fps;              // velocidad (cuadros por segundo)
    bool  crossfade;        // mezcla CPU entre celdas fraccionales (default false)
    // SUB-TIRA dentro de un atlas compartido (default 1,1 = la textura entera):
    // la grilla de celdas ocupa esta FRACCION de la textura. Con las tiras metidas
    // en el atlas unico, el paso por celda ya no es 1/cols sino ancho/cols. Las UV
    // base del quad se autoran en la celda 0 y el offset queda RELATIVO: el
    // consumidor Mesh (AnimacionUV `ancho:`/`alto:`) suma celda*paso a sus UV.
    float stripWidth, stripHeight;
    // ORIGEN de la grilla dentro del atlas (default 0,0 = esquina superior
    // izquierda). Con el ATLAS UNICO (todo el juego en una textura) la tira ya
    // no arranca en (0,0): la grilla vive en el rect [u0, u0+ancho] x [v0,
    // v0+alto]. Los consumidores que leen uvCurrent/RectOfCell lo heredan solos.
    float stripU0, stripV0;
    // 8 canales de UV del quad. Indice = esquina*2 + eje: 0=e0.u 1=e0.v 2=e1.u 3=e1.v
    // 4=e2.u 5=e2.v 6=e3.u 7=e3.v. Esquinas en orden (0,0)(1,0)(1,1)(0,1) del rect.
    AnimProperty uv[8];

    Flipbook();
    // UV (u0,v0,u1,v1) del rect de una celda segun cols/filas (celda 0..cols*filas-1)
    void RectOfCell(int cell, float& u0, float& v0, float& u1, float& v1) const;
    // autoria: en 'frame', deja el quad mostrando 'celda' (keyframes CONSTANTES en las 8 curvas)
    void KeyCell(int frame, int cell);
    // arma el flipbook CLASICO: celdas 0..cuadros-1, un cuadro por frame, interp constante
    void GenerateCells();
    // fija cols/filas/cuadros/fps (y la fraccion + origen de sub-tira) y
    // (re)genera el flipbook clasico de golpe
    void ConfigureStrip(const std::string& atlasPath, int cols, int rows, int frames, float fps,
                        float width = 1.0f, float height = 1.0f,
                        float u0 = 0.0f, float v0 = 0.0f);
};

// ---------------------------------------------------------------------------
//  FlipbookPlayer — estado de reproduccion de UN consumidor. Embebido en cada
//  Image2D / Mesh / emisor de particulas. Lee las curvas del Flipbook compartido
//  en su PROPIO tiempo. Se registra en el registro global (un solo tick para todos).
// ---------------------------------------------------------------------------
class FlipbookPlayer {
public:
    Flipbook* fb;        // asset compartido (NULL = sin animacion)
    int   offset;        // cuadro inicial (para desfasar copias del mismo flipbook)
    float accum;         // segundos acumulados (tiempo propio)
    float uvCurrent[8];  // la UV del quad AHORA (la lee el render del consumidor)
    int   currentFrame;  // celda entera actual (consumidores que saltan por celda)

    FlipbookPlayer();
    // engancha/reconfigura. registrar=true (default) lo mete en el registro global (lo tickea
    // UpdateFlipbooks): asi lo usan Image2D/particulas. Un Mesh pasa registrar=false y lo tickea
    // el mismo en su propio pase (escribe geometria, no solo lee uvCurrent). f=NULL desengancha.
    void Set(Flipbook* f, int offset = 0, bool doRegister = true);
    // avanza el tiempo y evalua fb->uv -> uvCurrent/currentFrame. true = cambio (pedir redraw).
    bool Tick(float dtSeg);
    ~FlipbookPlayer();
};

// registro global unico (funde gUVAnimMeshes + gImg2DAnim). Los players se auto-registran.
void FlipbookRegister(FlipbookPlayer* p);
void FlipbookRemove(FlipbookPlayer* p);
bool UpdateFlipbooks(float dtSeg);   // tickea todos los players; true si alguno cambio
bool HasFlipbooks();

// ---- Flipbooks CON NOMBRE de la escena (assets COMPARTIDOS: varios objetos usan el mismo) ----
// Aparecen en el selector de animaciones (categoria "Flipbooks" del timeline / tarjeta Animation).
extern std::vector<Flipbook*> SceneFlipbooks;
Flipbook* FlipbookByName(const std::string& n);   // NULL si no existe
Flipbook* NewFlipbook(const std::string& base);   // crea uno con nombre UNICO y lo agrega a la escena
void      FlipbookDelete(Flipbook* f);            // lo saca de la escena y lo libera
void      SceneFlipbooksClear();                  // vacia todo (al abrir / nueva escena)

#endif
