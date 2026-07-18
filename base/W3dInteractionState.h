#ifndef W3D_INTERACTIONSTATE_H
#define W3D_INTERACTIONSTATE_H

// Estado de INTERACCION de la app, COMPARTIDO motor<->editor y por eso vive en el MOTOR.
// Antes estas dos globales + sus enums colgaban de variables.h (header del EDITOR), y el
// traversal del Core incluia ese header -> invertia la capa (Motor <- UI <- Editor). Ahora
// el motor las declara y define; el editor las ESCRIBE segun el input, el Core las LEE
// (seleccion, gizmos). Dialecto C++03 (compila con RVCT en Symbian).

// modos del viewport (selector estilo Blender; solo con una MALLA activa). Edit y los Paint
// todavia no estan implementados: por ahora el selector solo cambia InteractionMode.
enum { ObjectMode, EditMode, VertexPaint, WeightPaint, TexturePaint, PoseMode };
// sub-estado del viewport: navegando, o en medio de un transform (G/R/S/extrude...).
enum { editNavegacion, EdgeMove, FaceMove, timelineMove, rotacion, EditScale, translacion };

extern int InteractionMode; // uno de ObjectMode/EditMode/...
extern int estado;          // uno de editNavegacion/...

#endif
