#ifndef W3D_LAYER_ANIM_H
#define W3D_LAYER_ANIM_H
#include <string>

// ============================================================================
//  UNA CAPA DEL MIX (estilo Maya/Unity): un clip que suena a la vez que otros, con su influencia y su modo.
//  El armature muestra la MEZCLA de sus capas visibles, en orden (la primera es la base):
//    Mezclar (0): la pose va hacia la de la capa en 'influencia' (lerp / slerp por hueso)
//    Sumar   (1): se le SUMA lo que la capa se aparta del reposo (una capa aditiva: respirar, apuntar...)
//    Restar  (2): se le resta
//  'hueso' la limita a ese hueso y sus hijos (las piernas caminan con una capa, el torso apunta con otra).
//  Jugando, cada capa tiene su cabezal (gameFrame) y la manejan los scripts (animCapa...); en el editor
//  (modo Mix del timeline) la capa arranca en el frame 'desde' del mix y avanza a 'vel'.
//  La misma capa sirve para las animaciones de ESCENA (g_mixScenes) y para los CLIPS DE JERARQUIA de una
//  raiz (W3dHierRoot::capas, animation/W3dAnimSet.h: 'anim' = el nombre del clip; sin mascara de hueso).
// ============================================================================
struct W3dLayerAnim {
    std::string anim;    // nombre del clip del armature (o de la animacion de escena, en las capas de escena)
    int   clipCache;     // indice resuelto de 'anim' (runtime: se revalida por nombre)
    float influence;     // % (0..100; sumar/restar aceptan hasta 200)
    int   mode;          // 0 mezclar, 1 sumar, 2 restar
    bool  visible;       // el "ojo": oculta la capa sin borrarla
    std::string bone;    // mascara: solo ese hueso y sus hijos ("" = todo el esqueleto)
    float from;          // frame del mix en el que arranca (editor)
    float vel;           // velocidad (1 = el FrameRate del clip)
    bool  loop;
    float gameFrame;   // JUEGO: frames desde el inicio del clip (runtime, no se guarda)
    bool  gameEnded;   // JUEGO: un clip sin loop llego al final
    // RETARGET "rotaciones" (armatures y raices de clips de jerarquia): la escala del movimiento de la raiz para
    // el clip 'retClip' (cache: se recalcula si la capa cambia de clip). Runtime, no se guarda.
    const void* retClip;
    float retScale;
    W3dLayerAnim() : clipCache(-1), influence(100.0f), mode(0), visible(true), from(0.0f), vel(1.0f), loop(true),
                    gameFrame(0.0f), gameEnded(false), retClip(0), retScale(1.0f) {}
};

#endif // W3D_LAYER_ANIM_H
