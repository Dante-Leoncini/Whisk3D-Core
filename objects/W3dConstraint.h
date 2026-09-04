#ifndef W3D_CONSTRAINT_H
#define W3D_CONSTRAINT_H

#include <string>
#include "math/Vector3.h"

// ============================================================================
//  UN CONSTRAINT ES UNA PROPIEDAD DEL OBJETO, NO UN OBJETO
//
//  Antes "el constraint" era un nodo mas del arbol (main/objects/Constraint.h) que
//  apuntaba al objeto que queria modificar y le ESCRIBIA la rotacion en el render.
//  Escribir el campo real es lo que hacia que el efecto se HORNEARA: el .w3d guarda
//  'rotEuler', asi que guardar despues de dibujar dejaba la orientacion de la ultima
//  camara metida adentro del archivo. Y con dos viewports abiertos ganaba el ultimo
//  que dibujo.
//
//  Ahora el constraint VIVE EN el objeto que modifica y el puntero es la FUENTE. 
//  No escribe nada: se
//  evalua al vuelo dentro de Object::GetMatrix, o sea POR VISTA, y los campos
//  pos/rot/rotEuler/scale del objeto quedan siempre como el usuario los dejo. Lo que
//  hornea geometria o escribe a disco usa la variante *Base, que ni mira esta lista.
//
//  NO es thread-safe (la guardia de ciclos 'consEval' es un bool por objeto). Hoy el
//  render es monohilo; si alguna vez deja de serlo, esto se replantea.
// ============================================================================

class Object;   // solo forward: la fuente es un Object*

struct W3dConstraintTipo { enum Enum { CopyLocation = 0, CopyRotation = 1, Billboard = 2 }; };
struct W3dConstraintFuente { enum Enum { Objeto = 0, Vista = 1 }; };

// ============================================================================
//  IDENTIDAD ESTABLE DE UN CONSTRAINT (calcado de W3dModSerial, main/edit/Modifier.h:36)
//
//  Un W3dConstraint vive en el heap y el stack lo borra con delete: la direccion se
//  RECICLA, asi que nada que sobreviva a la lista (el undo del borrado de objetos)
//  puede identificar una entrada por su puntero. Por el indice tampoco: Add / Remove /
//  Move Up / Move Down reordenan la lista y el indice guardado pasa a nombrar a OTRO
//  constraint, en silencio. Por eso cada uno nace con un numero MONOTONO que no se
//  reusa nunca.
//
//  El serial NO se copia: duplicar un objeto da OTROS constraints, y darles el mismo
//  numero seria justo el reciclaje que esto evita. Va en un tipo propio para que el
//  copy-constructor IMPLICITO de W3dConstraint haga lo correcto solo.
// ============================================================================
struct W3dConSerial {
    unsigned int v;
    W3dConSerial();                     // v = el proximo del contador. Nunca es 0.
    W3dConSerial(const W3dConSerial&);  // una COPIA es otro constraint -> numero NUEVO
    W3dConSerial& operator=(const W3dConSerial&) { return *this; } // idem: no se pisa
};

// "Copy Location" / "Copy Rotation" / "Billboard"
// (gemelo de NombreTipoModificador, main/edit/Modifier.h:105). Se define en
// libs/Whisk3DCore/objects/Objects.cpp: este header no tiene .cpp propio a proposito
// (un .cpp nuevo hay que agregarlo a las 9 listas de fuentes del proyecto).
const char* W3dNombreTipoConstraint(int tipo);

class W3dConstraint {
    public:
        W3dConSerial serial;    // identidad estable (ver arriba). 'serial.v' nunca es 0.

        int          tipo;      // W3dConstraintTipo::Enum
        std::string  nombre;    // lo que muestra la lista; regla de nombres unicos POR OBJETO
        bool         activo;    // el "ojito" de la lista

        // ================================================================================
        //  VER EN MODO EDICION (lo pidio el dueno, textual: "si el constraint tiene tildado
        //  'ver en modo edicion' se ve en modo edicion. sino, dejame editarlo en su posicion,
        //  rotacion, escala real porque sino se hace muy dificil").
        //  Cuando esta APAGADO, el objeto que se esta editando dibuja y se edita con su
        //  transform REAL (la que muestra la pestania Objeto y la que va al .w3d): un arbol
        //  con billboard deja de perseguir a la camara mientras uno le mueve los vertices.
        //  Solo afecta al objeto EN EDICION, no a sus padres ni a sus hermanos: apagar el
        //  constraint de un padre moveria tambien a los otros hijos, que nadie pidio.
        //  ARRANCA EN OFF, al reves que el 'mostrarEdit' de los modificadores (Modifier.h),
        //  y a proposito: un modificador que no se ve en Edit Mode esconde geometria, pero un
        //  constraint que no se aplica en Edit Mode es justamente la comodidad que se pidio.
        // ================================================================================
        bool         mostrarEdit;

        // La influencia se guarda y se edita en 0..100 (el panel la muestra con "%"), no
        // en 0..1: PropFloat bindea un float* DIRECTO al campo, asi que un rango 0..1
        // obligaria al proxy global de Properties.cpp (que se rompe con dos paneles
        // abiertos). Rompe la convencion de Opacity (0..1) a proposito.
        float        influencia;
        // 0..1 para las formulas. Los extremos son EXACTOS por CLAMP explicito: 'influencia'
        // viene de un campo editable y de un archivo, asi que 99.9998 no puede dar 1.0
        // (el objetivo se aplicaria "casi" y no se veria igual que el objeto viejo) y
        // 100.0001 no puede pasarse.
        float Peso() const {
            if (influencia <= 0.0f) return 0.0f;
            if (influencia >= 100.0f) return 1.0f;
            return influencia * 0.01f;
        }

        // ---- FUENTE (Copy Location / Copy Rotation) ----
        // 'Vista' es la camara que esta dibujando, y es un tipo EXPLICITO: si "NULL
        // significa la vista", borrar el objeto fuente (que por diseno deja el puntero en
        // NULL) convertiria en silencio un Copy Location en un "seguir a la camara".
        int          fuenteTipo;    // W3dConstraintFuente::Enum
        // puntero VIVO, no se serializa. NULL => el constraint no hace nada.
        // Lo enumera Object::RefsObjeto (Objects.h), asi que el destructor y el undo lo
        // sueltan solos cuando el apuntado se borra.
        Object*      fuenteObj;
        std::string  fuenteNombre;  // el vinculo POR NOMBRE: ESTO es lo que se guarda

        // ---- EJES (Copy Location / Copy Rotation) ----
        bool         ejeX, ejeY, ejeZ;

        // ---- BILLBOARD ----
        bool         bbYaw;         // gira en horizontal
        bool         bbPitch;       // ademas se inclina
        // "tipo arbol"      = bbYaw true,  bbPitch false
        // "mira a la vista" = bbYaw true,  bbPitch true
        // (!bbYaw && bbPitch) es legible del formato viejo y se evalua bien, pero NO es
        // alcanzable desde la UI. (!bbYaw && !bbPitch) NO EXISTE: un billboard que no hace
        // nada no puede ser representable, lo arregla Normalizar().
        //
        // OJO, conducta conocida y ACEPTADA: con influencia intermedia (ej. 50%) el
        // billboard pega un giro de 180 grados cuando la diferencia de angulo con la
        // camara cruza los 180. Es inherente a mezclar contra una orientacion ABSOLUTA
        // que gira; suavizarlo pide acumular vueltas, o sea estado POR VISTA, que es
        // justo lo que este diseno elimina.

        // aviso de ciclo (A copia de B y B de A): se prende al avisar para no inundar la
        // pantalla con una notificacion POR FRAME. Runtime puro, no se guarda. Significa
        // "ya avise para ESTA fuente": el panel lo vuelve a poner en false cada vez que se
        // elige otra fuente (AccionConFuenteElegida), o sea que romper el ciclo y volver a
        // armarlo avisa de nuevo.
        bool         avisoCiclo;

        // C++03: nada de inicializadores en la clase, todo en la lista del ctor.
        explicit W3dConstraint(int t)
            : tipo(t),
              nombre(W3dNombreTipoConstraint(t)),
              activo(true),
              mostrarEdit(false),   // OFF: en Edit Mode el objeto se edita en su transform real
              influencia(100.0f),
              fuenteTipo(W3dConstraintFuente::Objeto),
              fuenteObj(NULL),
              fuenteNombre(),
              ejeX(true), ejeY(true), ejeZ(true),
              bbYaw(true), bbPitch(false),
              avisoCiclo(false) {}

        // deja los campos en un estado que el evaluador y la UI puedan representar. La
        // llama el lector del .w3d, que es el unico que trae valores de afuera (un archivo
        // lo pudo editar cualquiera). El panel NO la llama y no la necesita: el desplegable
        // de modo prende bbYaw a mano y el campo de influencia ya nace con SetRango(0,100).
        void Normalizar() {
            if (influencia < 0.0f)   influencia = 0.0f;
            if (influencia > 100.0f) influencia = 100.0f;
            if (tipo == W3dConstraintTipo::Billboard && !bbYaw && !bbPitch) bbYaw = true;
        }
};

#endif // W3D_CONSTRAINT_H
