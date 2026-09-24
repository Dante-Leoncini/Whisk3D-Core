#ifndef W3D_CAPA_ANIM_H
#define W3D_CAPA_ANIM_H
#include <string>

// ============================================================================
//  UNA CAPA DEL MIX (estilo Maya/Unity): un clip que suena a la vez que otros, con su influencia y su modo.
//  El armature muestra la MEZCLA de sus capas visibles, en orden (la primera es la base):
//    Mezclar (0): la pose va hacia la de la capa en 'influencia' (lerp / slerp por hueso)
//    Sumar   (1): se le SUMA lo que la capa se aparta del reposo (una capa aditiva: respirar, apuntar...)
//    Restar  (2): se le resta
//  'hueso' la limita a ese hueso y sus hijos (las piernas caminan con una capa, el torso apunta con otra).
//  Jugando, cada capa tiene su cabezal (juegoFrame) y la manejan los scripts (animCapa...); en el editor
//  (modo Mix del timeline) la capa arranca en el frame 'desde' del mix y avanza a 'vel'.
// ============================================================================
struct W3dCapaAnim {
    std::string anim;     // nombre del clip del armature (o de la animacion de escena, en las capas de escena)
    int   clipCache;      // indice resuelto de 'anim' (runtime: se revalida por nombre)
    float influencia;     // % (0..100; sumar/restar aceptan hasta 200)
    int   modo;           // 0 mezclar, 1 sumar, 2 restar
    bool  visible;        // el "ojo": oculta la capa sin borrarla
    std::string hueso;    // mascara: solo ese hueso y sus hijos ("" = todo el esqueleto)
    float desde;          // frame del mix en el que arranca (editor)
    float vel;            // velocidad (1 = el FrameRate del clip)
    bool  loop;
    float juegoFrame;     // JUEGO: frames desde el inicio del clip (runtime, no se guarda)
    bool  juegoTermino;   // JUEGO: un clip sin loop llego al final
    W3dCapaAnim() : clipCache(-1), influencia(100.0f), modo(0), visible(true), desde(0.0f), vel(1.0f), loop(true),
                    juegoFrame(0.0f), juegoTermino(false) {}
};

#endif // W3D_CAPA_ANIM_H
