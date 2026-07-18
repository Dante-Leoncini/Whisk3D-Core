# Assets protegidos — modelo de amenaza (honesto)

Whisk3DCore puede empaquetar imágenes, sonido y video en un contenedor **cifrado**
(`.w3dpack`, ver `io/W3dPack.h`) y descifrarlo **en memoria** para alimentar al decoder,
sin que nunca exista un archivo con nombre ni una URL plana. El objetivo es **honesto y
acotado**, y conviene decirlo tal cual a los creadores:

> **No existe "100% imposible de robar"** para nada que se reproduzca en un cliente que el
> usuario controla (navegador, PC). El frame decodificado tiene que llegar a la pantalla y
> el audio al parlante → siempre se puede capturar. Lo que damos es **disuasión fuerte**:
> hacer el robo caro y molesto, no imposible.

## Qué SÍ frena (el ~99%)

- **Copiar y pegar del inspector**: en el navegador no hay ningún `.mp4`/`.webm`/`.png` con
  nombre. La pestaña de red solo muestra el pack cifrado (bytes ilegibles) o el `.wasm/.data`.
- **"Guardar como"**: los `<video>`/imágenes se usan como fuente de textura y no están en el
  DOM inspeccionable; el usuario solo ve el canvas WebGL (un "guardar" da un PNG de un frame,
  no el asset). El blob interno del video se **revoca** apenas carga.
- **Scripts triviales** que bajan URLs o listan archivos: no hay archivos que listar.

## Qué NO frena (y hay que asumirlo)

- **Alguien que lea el binario/wasm**: si la clave va **embebida** en la app, quien la
  extraiga descifra el pack. Es el **principio de Kerckhoffs**: la seguridad vive en la
  *clave*, no en el secreto del código. Clave embebida ⇒ *disuasión*, no garantía.
- **Un usuario autorizado**: si puede reproducirlo, su cliente recibe/tiene la clave; con un
  dump de memoria o grabando la pantalla se lleva su copia. Nadie evita esto (ni Netflix).

## Custodia de la clave — el único parámetro que sube el nivel

La interfaz del Core es la misma; lo que cambia es **de dónde sale la clave** que la app le
pasa a `W3dPack::Open`:

| Custodia | Resiste a… | No resiste a… | Necesita |
|---|---|---|---|
| **Clave embebida** (deterrence) | inspector, "guardar como", scripts | quien lee el binario | nada (self-contained, MIT) |
| **Clave de servidor por sesión** | además, quien solo tiene el código público | el usuario autorizado que ripea su copia | un servidor de claves del creador |

Para el modo fuerte, la app no embebe la clave: la pide a **su** servidor tras autenticar la
sesión. Con solo el código público (sin sesión válida) no se puede descifrar. El cliente
sigue siendo MIT; el servidor es del creador.

## Cifrado

`.w3dpack` usa **ChaCha20** (cifrador de flujo, verificado contra su vector conocido). La
fuerza del cifrado no cambia lo de arriba: si la clave está en el cliente, el cifrado no
salva. Lo que protege es **la custodia de la clave**, no el algoritmo.
