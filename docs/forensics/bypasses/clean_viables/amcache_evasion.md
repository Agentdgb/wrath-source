# Bypass Limpio: Evasión de AmCache (Amcache.hve)

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass neutraliza la indexación y correlación de hashes en `Amcache.hve` sin requerir la eliminación destructiva de la colmena ni la alteración de servicios de telemetría.

---

## 1. Vector Principal: Salt Criptográfico por Compilación (Hash Polymorphism)
*Ubicación en el código:* `Bypasses/tools/gen_salt.ps1` y `Bypasses/strings/build_salt.h`.

1. **El Problema del Hash SHA-1**:
   - AmCache indexa el hash SHA-1 de cada binario analizado. Si un loader se distribuye de forma estática, su hash SHA-1 es idéntico para todos los usuarios y los escáneres (Echo, Paladin, Ocean) lo detectan al instante en su blacklist.
2. **Solución Wrath / VANTA**:
   - El script `gen_salt.ps1` genera un valor aleatorio criptográfico de 64 bits (`RandomNumberGenerator`) inyectado en `build_salt.h` antes de cada compilación:
     ```cpp
     #define BUILD_SALT 0x4F8A1C93B2D6E701ULL
     ```
   - Este salt altera la estructura de datos en `.rdata` / `.text`, modificando por completo el hash SHA-1 del ejecutable resultante.
   - **Resultado**: Cero coincidencias en bases de datos de hashes de screenshares. Cada compilación es matemáticamente única.

---

## 2. Vector Secundario: Mimetización de Recursos PE de Sistema
*Ubicación en el código:* `Bypasses/loader_stealth/version.rc`.

1. **Evasión de `UnassociatedFileEntries`**:
   - `AmcacheParser` agrupa los ejecutables sin metadatos legítimos como sospechosos.
   - WrathLoader integra un script de recursos de versión (`version.rc`) que clona fielmente la información de binarios esenciales de Windows (e.g. `WerFault.exe` de Microsoft Corporation):
     - `CompanyName`: `"Microsoft Corporation"`
     - `FileDescription`: `"Windows Problem Reporting"`
     - `FileVersion`: `"10.0.19041.3636"`
     - `LegalCopyright`: `"\xA9 Microsoft Corporation. All rights reserved."`
   - Al ser indexado por AmCache, el archivo se clasifica como una utilidad legítima del sistema operativo con metadatos completos y coherentes.

---

## 3. Aislamiento por VHD Volátil y Ciclo de Vida Desfasado
*Ubicación en el código:* `Bypasses/tools/setup_vhd_environment.ps1`.

1. **Momento de Indexación de AmCache**:
   - Windows no indexa inmediatamente cada binario en `Amcache.hve`. La tarea programada `Microsoft\Windows\Application Experience\ProgramDataUpdater` y el servicio `PcaSvc` procesan las aplicaciones en lotes periódicos, durante periodos de inactividad (`idle`) o durante el apagado.
2. **Montaje y Desmontaje Oculto**:
   - El ejecutable reside en el contenedor VHD montado sin letra de unidad (`NON_PNP`).
   - El loader se ejecuta, inyecta `WrathPayload.dll` en `javaw.exe` y finaliza en milisegundos.
   - El contenedor VHD se desmonta limpiamente. Cuando la tarea programada de Windows corre o se analiza `C:\Windows\AppCompat\Programs\Amcache.hve`, la ruta virtual ya no existe en el sistema de archivos del host, impidiendo que AmCache complete la lectura del archivo o asociando el registro al contenedor general `Backup_2026.vhd`.

---

## 4. Inyección en Memoria del Payload (`WrathPayload.dll`)
1. `WrathPayload.dll` no se registra como una aplicación independiente en `InventoryApplicationFile`.
2. Las bibliotecas DLL mapeadas manualmente o stompeadas en el espacio de memoria de un proceso anfitrión (`javaw.exe`) no generan entradas de aplicación individual en `Amcache.hve`.
