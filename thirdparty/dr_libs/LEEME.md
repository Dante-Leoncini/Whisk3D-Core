# dr_libs (solo dr_mp3)

`dr_mp3.h` — decodificador de MP3 de un solo header, de David Reid (basado en minimp3 de lieff).

- Version: **v0.7.4** (2026-10-31), tal cual, sin cambios.
- Origen: https://github.com/mackron/dr_libs/blob/aec2dd20667f54acfdb66031217df6d240f56c52/dr_mp3.h
- SHA-256: `f5c8020eeebb79877184432efdd2651f24869f5bb8f379d45450fd0aed1ee915`
- Licencia: a eleccion, **dominio publico (Unlicense) o MIT-0** (ver el final del archivo). Sin atribucion obligatoria.

Lo usa `audio/W3dAudio.cpp` (`DR_MP3_IMPLEMENTATION` + `DR_MP3_NO_STDIO`: decodifica desde memoria, sin abrir
archivos) para que el objeto Sonido y `sonido()` de lua acepten `.mp3` ademas de `.wav`. No entra en Symbian (ahi el
mp3 lo decodifica el hardware por `W3dMusic`, y un tema entero en PCM no entra en la RAM del N95).
