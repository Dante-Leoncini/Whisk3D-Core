#include "W3dInteractionState.h"

// Valores iniciales: modo objeto, navegando (los mismos 0 que daba la inicializacion
// por defecto de globales cuando vivian en variables.cpp).
int InteractionMode = ObjectMode;
int state = editNavigation;

// render FINAL (foto/pases): los objetos no-renderizables se saltean. Definido aca
// porque este .cpp lo linkean TODAS las plataformas (el backend GL varia)
namespace w3dEngine { bool w3dRenderFinal = false; bool w3dPassProbe = false; unsigned w3dDrawsLost = 0; }

// pase de ESPEJO sin z-test (ver w3dGraphics.h): tambien aca, por la misma razon
namespace w3dEngine { bool w3dPassMirrorNoZ = false; }
