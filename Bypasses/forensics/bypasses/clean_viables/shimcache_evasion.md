# Bypass Limpio: Evasión de ShimCache (AppCompatCache)

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass garantiza que ningún ejecutable del cliente figure en `AppCompatCache` como ejecutable sospechoso o borrado, sin corromper el valor binario del registro del sistema.

---

## 1. Naturaleza Intrínseca: DLLs e Inyección en Memoria
*Regla de diseño de Wrath / VANTA:*

1. **ShimCache NO registra bibliotecas DLL**:
   - La infraestructura de compatibilidad de aplicaciones de Windows (`AppCompat`) está diseñada exclusivamente para procesos ejecutables (`.exe`).
   - `WrathPayload.dll` se inyecta en el proceso de Minecraft (`javaw.exe`) mediante manual mapping o module stomping.
   - **Resultado**: El payload **nunca ingresa en el ShimCache**. En el registro de Windows no existirá jamás una entrada para `WrathPayload.dll`.

---

## 2. Vector del Loader: Mimetización de Binario y Ruta VHD
*Ubicación en el código:* `Bypasses/tools/setup_vhd_environment.ps1` y `loader_stealth`.

1. **Nombre e Identidad Señuelo**:
   - El cargador no se compila con nombres genéricos de cheats (`loader.exe`, `wrath.exe`).
   - Se compila y nombra como una utilidad legítima de mantenimiento o controladores de pantalla (e.g. `DisplayTunerService.exe` o `BackupManager.exe`).
2. **Ubicación en Carpeta de Sistema Señuelo**:
   - Se ubica en la ruta enlazada del VHD o carpeta administrativa (`%LOCALAPPDATA%\Microsoft\Windows\SystemDataCache\Tools\BackupManager.exe`).
   - Si `AppCompatCacheParser` analiza el sistema:
     - La entrada aparece como una utilidad de respaldo legítima del sistema.
     - Su marca de tiempo (`LastModifiedTime`) coincide con las marcas clonadas de archivos nativos de Windows (`robocopy.exe`, `xcopy.exe`).
     - No aparece como una aplicación anómala ni en directorios sospechosos de descargas.

---

## 3. Preservación del Archivo de Señuelo en Disco (Evitar Flag de Archivo Borrado)
1. **El Problema**:
   - Una de las principales firmas que busca Echo/Paladin en ShimCache es:
     *Entrada con `Executed = True` pero `File Exists = False` en disco*.
2. **Solución Wrath**:
   - El binario señuelo (`BackupManager.exe`) **NO se borra de forma precipitada** si el entorno VHD continúa montado durante la sesión.
   - Si el señuelo permanece en disco con su hash y estructura PE válida durante el tiempo que dura la sesión, el escáner verifica que el archivo existe, sus marcas de tiempo coinciden y no lo marca como archivo borrado encubierto.
   - Al desmontar el VHD al apagar o cerrar la sesión, el contenedor completo se retira de manera atómica sin dejar rastros huérfanos individuales.
