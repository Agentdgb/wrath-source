# Bypass Obsoleto / Blatant: DLL Hijacking Clásico y DLL Proxying en Disco

## Nivel: RECHAZADO / OBSOLETO / BLATANT

El método más quemado, arcaico y vulnerable en el ámbito de screenshares de Minecraft y juegos protegidos.

---

## 1. Colocar DLLs Falsas (`version.dll`, `dxgi.dll`, `opengl32.dll`) en la Carpeta del Juego
- **Técnica errónea**:
  Copiar un cheat compilado como `version.dll` en `.minecraft/` o en el directorio `bin` del JRE (`javaw.exe`), confiando en que Windows la cargará por precedencia en el Search Order.
- **Por qué falla (Detección al 100%)**:
  1. **Auditoría de Certificados en System Informer**: System Informer verifica el estado de firma digital de cada módulo en el proceso. Una DLL `version.dll` no firmada por Microsoft o ubicada fuera de `System32` salta en color rojo/púrpura de inmediato.
  2. **Comprobación Estática de Echo / Paladin**: Los escáneres automáticos inspeccionan la totalidad de archivos en la carpeta de Minecraft y del runtime de Java antes de que el jugador inicie sesión. Cualquier DLL presente en esas carpetas que no coincida con el hash de la distribución oficial de Mojang o no esté firmada genera un **Ban Inmediato por Fake DLL**.
  3. **Persistencia en MFT / $LogFile**: Dejar el archivo DLL en el disco deja marcas de tiempo, transacciones en `$LogFile` y registros en `$MFT`. Si el usuario intenta borrar la DLL durante el juego, Windows bloqueará la acción (`Sharing Violation`) porque el proceso la tiene abierta.

---

## 2. DLL Proxying con Export Forwarding Incompleto
- **Técnica errónea**: Crear un proxy de una DLL del sistema redirigiendo solo las funciones que el creador cree necesarias.
- **Por qué falla**:
  - Si el proceso o algún sub-componente de Java intenta importar un ordinal o nombre de función no exportado por el proxy, la aplicación crashea con el error `Entry Point Not Found`.
  - Windows registra el Evento 1000 (Application Crash) en el log de eventos `Application.evtx`, entregando evidencia al revisor de que una DLL corrupta intentó cargarse.

---

## 3. Desvincular la DLL del PEB sin Ocultar VAD
- **Técnica errónea**: Cargar la DLL normalmente con `LoadLibrary` y luego recorrer `PEB->Ldr` para "desvincularla" (`UnlinkFromLdr`).
- **Por qué falla**:
  - Herramientas forenses modernas no confían en el PEB (es estructura en userland fácilmente modificable).
  - Los escáneres consultan el **VAD** (Virtual Address Descriptors) en el kernel o mediante `VirtualQueryEx`.
  - Si el VAD indica que hay páginas ejecutables asignadas (`MEM_IMAGE`) pero el PEB no contiene ningún módulo en esa dirección base, se detecta de inmediato como **Unlinked / Hidden Injected Module**.
