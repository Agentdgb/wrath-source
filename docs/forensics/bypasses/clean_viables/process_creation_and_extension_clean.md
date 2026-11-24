# Bypass Limpio: Creación Nativa y Descarte de Extensiones Falsas

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass asegura que el proceso de carga del cliente no genere cadenas sospechosas de WMIC en la memoria de los procesos del sistema (`Explorer.exe`, `SgrmBroker.exe`, `MsMpEng.exe`) y descarta por completo el uso de extensiones disfrazadas que activan alertas en Prefetch y DiE.

---

## 1. Vector Principal: Cero Invocación de WMI / LOLBins
*Regla de arquitectura de Wrath / VANTA:*

1. **Invocación Nativa sin WMI ni Scripts**:
   - `WrathLoader.exe` jamás se ejecuta invocando `wmic process call create ...` ni scripts de PowerShell `Start-Process`.
   - Se inicia directamente mediante el subsistema Win32 nativo (`CreateProcessW`) con los flags de creación estándar.
2. **Resultado en la Memoria de Procesos del Sistema**:
   - En `Explorer.exe`: No existe la cadena `"wmic process call create"`.
   - En `SgrmBroker.exe`: Cero registros de llamadas anómalas de creación de procesos WMI.
   - En `MsMpEng.exe` (Windows Defender): Los buffers de inspección de comportamiento en memoria no contienen trazas de líneas de comando de WMI.
   - Una búsqueda de la palabra `"wmic"` en los volcados de memoria de estos tres procesos devuelve cero coincidencias vinculadas al cliente.

---

## 2. Rechazo de Spoofed Extensions: Estructura PE Legítima y Mimetización
*Ubicación en el código:* `Bypasses/loader_stealth/version.rc` y `Bypasses/tools/setup_vhd_environment.ps1`.

1. **Por Qué se Descarta Renombrar a `.png` o `.txt`**:
   - Renombrar un ejecutable a `.png` es una técnica arcaica que garantiza detección inmediata (crea archivos `.pf` delatores como `LOGO.PNG.pf` y salta en búsquedas de `"This program cannot be run in DOS mode."`).
2. **El Estándar Wrath**:
   - **El Payload (`WrathPayload.dll`)**:
     - No reside en disco con extensiones falsas. Se inyecta en memoria RAM de `javaw.exe` vía Manual Mapping (`ManualMap.h`) o transacciones TxF. Cero archivos en disco, cero entradas en Prefetch.
   - **El Loader (`WrathLoader.exe`)**:
     - Se mantiene con la extensión legítima `.exe`.
     - Se nombra como una utilidad administrativa nativa creíble (`DisplayTunerService.exe` o `BackupManager.exe`).
     - Integra recursos de versión auténticos clonados de Microsoft (`version.rc`: `WerFault.exe` / `Microsoft Corporation`).
     - Si genera un archivo Prefetch, este tiene el nombre legítimo de una herramienta utilitaria (`DISPLAYTUNERSERVICE.EXE-XXXXXXXX.pf`), con estructura PE válida, sin discrepancias en Detect It Easy (DiE) y sin activar filtros de extensiones anómalas en `csrss.exe` o `dps`.

---

## 3. Aislamiento en Contenedor VHD (M08)
1. El ejecutable del loader reside en el contenedor VHD montado sin letra de unidad (`NON_PNP`).
2. Cuando el analista ejecuta Search Everything buscando `"This program cannot be run in DOS mode."` en las unidades del usuario, los archivos internos del VHD no figuran en las carpetas de usuario del host `C:\`.
