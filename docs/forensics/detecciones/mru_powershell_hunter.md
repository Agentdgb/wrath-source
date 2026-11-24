# Detección Forense: MRUHunt y PowerShell-Hunter (MRU, JumpLists, RecentApps)

## 1. Identificación y Alcance de las Herramientas
- **Herramientas**: Suite `PowerShell-Hunter` (creada por Michael Haggis / MHaggis):
  - `Analyze-MRURegistry.ps1`
  - `JumpListAnalysis.ps1`
  - `RecentApps.ps1`
- **Objetivo**: Escaneo y reconstrucción forense de la actividad manual del usuario mediante artefactos MRU (Most Recently Used), listas rápidas de Windows Shell (Jump Lists) y registro de comandos ejecutados en el sistema.
- **Ámbito de uso**: Muy extendido en revisiones de FiveM, Minecraft competitivo y auditorías de screenshares manuales.

---

## 2. Artefactos y Ubicaciones Inspeccionadas

### A. MRU del Registro (Analyze-MRURegistry.ps1)
1. **RunMRU (Comandos de la ventana Ejecutar `Win + R`)**:
   - Ruta: `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\RunMRU`
   - Almacena lo que el usuario escribió manualmente en el cuadro `Ejecutar`.
   - Cada comando se guarda con un valor alfabético (`a`, `b`, `c`, etc.) que incluye la ruta y el sufijo `\1`.
   - El valor `MRUList` determina el orden de uso (e.g. `"cba"` significa que `c` fue el más reciente).
2. **TypedPaths (Barra de Direcciones de Windows Explorer)**:
   - Ruta: `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\TypedPaths`
   - Guarda las rutas de carpetas que el usuario escribió manualmente en el Explorador para navegar (e.g., `C:\Users\Admin\AppData\Local\Temp\Loader`).
3. **RecentDocs**:
   - Ruta: `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\RecentDocs`
   - Historial de archivos abiertos o manipulados recientemente clasificados por extensión (`.exe`, `.bat`, `.zip`, `.dll`).

### B. Jump Lists (JumpListAnalysis.ps1)
- Rutas:
  - `%APPDATA%\Microsoft\Windows\Recent\AutomaticDestinations`
  - `%APPDATA%\Microsoft\Windows\Recent\CustomDestinations`
- Archivos con extensión `.automaticDestinations-ms` y `.customDestinations-ms` (formato binario Structured Storage / OLE CFB).
- **Qué contienen**: Cada aplicación anclada a la barra de tareas (o ejecutada frecuentemente) genera un ID de Jump List que almacena accesos directos (`LNK`) a archivos abiertos, ejecutados o descargados por esa aplicación.

### C. RecentApps (RecentApps.ps1)
- Ruta: `HKCU\Software\Microsoft\Windows\CurrentVersion\Search\RecentApps`
- Registra aplicaciones ejecutadas recientemente con su ruta completa, número de accesos y marca de tiempo `LastAccessedTime`.

---

## 3. Heurísticas y Reglas de Flag en Screenshares
- **Flag 1 — Comandos de Inyección en RunMRU**: Entradas que apunten a ejecutables sospechosos, `.bat` de limpieza, comandos de PowerShell o lanzadores portátiles (`inject.exe`, `spoof.bat`, `loader.exe`).
- **Flag 2 — Rutas Navegadas en TypedPaths**: Rutas navegadas manualmente hacia carpetas ocultas, VHDs o directorios temporales.
- **Flag 3 — JumpLists de Editores o Consolas**: Registros de Notepad, CMD o PowerShell abriendo scripts de cheats o archivos `.json`/`.cfg` de configuración de trampas.
- **Flag 4 — Claves MRU o Carpetas de Jump Lists Vacías**: Si `RunMRU` no existe o la carpeta `AutomaticDestinations` fue vaciada recientemente, se genera una alerta inmediata por **Destrucción de Historial (Tampering)**.
