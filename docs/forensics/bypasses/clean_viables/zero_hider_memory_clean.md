# Bypass Limpio: Filosofía Zero-Hider y Cifrado Total en Memoria

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass neutraliza ambos métodos de escaneo: erradica al 100% las cadenas de trampas en el proceso del juego mediante cifrado polimórfico en tiempo de compilación y descarta por completo el uso de herramientas hider que dejan huellas en los procesos del sistema.

---

## 1. Vector 1: Evasión en la Memoria del Proceso del Juego (`enc.h`)
*Ubicación en el código:* `Bypasses/core/enc.h` (`StackString`) y `wrath/client/`.

Para que el escaneo con HxD en el volcado de `javaw.exe` devuelva cero coincidencias ante palabras como `menu`, `injector`, `aimbot`, `reach`:

1. **Cero Cadenas Literales en el Payload**:
   - `WrathPayload.dll` compila la totalidad de cadenas de texto (nombres de funciones JNI, títulos de ClickGUI, identificadores de módulos y constantes) utilizando la macro `WRATH_STACK_STRING`:
     ```cpp
     auto name = WRATH_STACK_STRING("Aimbot");
     ```
   - El algoritmo mezcla la clave base con el salt aleatorio de compilación `BUILD_SALT` (`mix_key`), almacenando un arreglo de bytes en `.rdata` con alta entropía criptográfica.
2. **Cero Texto Plano en Memoria Estática**:
   - La cadena descifrada solo existe en los registros de CPU o en la pila local durante los microsegundos exactos de la llamada de renderizado o hook.
   - Tan pronto como la función retorna, el marco de pila se destruye y la memoria se limpia con `SecureZeroMemory` (`StringScrub.h`).
3. **Eliminación Total de Debug Strings**:
   - Todas las llamadas a `printf`, logs de consola, nombres de archivo fuente (`__FILE__`) y aserciones (`assert`) se compilan fuera del binario (`NDEBUG` y macros vacías en builds Release).
4. **Resultado en HxD**:
   - Al abrir el dump del juego en HxD y buscar `menu`, `reach`, `aimbot` o `velocity`, las únicas cadenas halladas corresponden a nombres legítimos del código vanilla de Minecraft / Fabric y sus librerías estándar.

---

## 2. Vector 2: Filosofía "Zero-Hider" (Inmunidad en `explorer.exe` y `csrss.exe`)
*Ubicación en el código:* `Bypasses/tools/setup_vhd_environment.ps1`.

1. **Rechazo Total de Usermode Hiders**:
   - Wrath / VANTA **jamás utiliza ejecutables de ocultamiento de archivos** ni inyecta hooks en `explorer.exe` ni `csrss.exe`.
   - Utilizar un hider es un error garrafal porque la presencia de la herramienta en memoria del sistema es en sí misma prueba de trampa.
2. **Aislamiento por Capa Virtual (VHD M08)**:
   - En lugar de intentar ocultar archivos en el Explorador de Windows, los binarios residen en el volumen virtual VHD montado sin letra de unidad (`NON_PNP`).
   - El usuario no necesita esconder carpetas en Windows Explorer porque en las carpetas comunes del usuario (`Desktop`, `Downloads`, `AppData`) no hay ningún archivo de cheat presente.
3. **Resultado en HxD sobre `explorer.exe` y `csrss.exe`**:
   - Al generar el dump de `explorer.exe` y `csrss.exe` y buscar palabras clave como `hide`, `hider` o `hidden`, **no existe ninguna traza de comandos de ocultamiento**.
   - Los procesos del sistema conservan una memoria 100% limpia sin handles ni hilos anómalos.
