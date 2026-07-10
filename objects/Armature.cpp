#include "objects/Armature.h"

// El Armature en el CORE es SOLO datos (huesos + jerarquia). El dibujo del esqueleto (lineas azules por encima de
// todo) es un EXTRA del editor: se hace en una pasada aparte DESPUES de renderizar la escena, para que las lineas
// queden encima del mesh hijo (si se dibujara aca, durante el recorrido del arbol, el mesh hijo -que se renderiza
// despues- las taparia). Ver RenderArmaturasEncima() en el editor.
void Armature::RenderObject() {}
