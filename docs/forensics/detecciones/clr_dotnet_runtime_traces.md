# Detección Forense: Trazas de Ejecución de .NET Runtime (CLR Logs)

## 1. Identificación y Alcance del Vector
- **Componente**: Common Language Runtime (CLR / .NET Framework / .NET Core / .NET 6/7/8).
- **Relevancia Forense**: Gran parte de los loaders, inyectores, clickers y herramientas de trampas privadas están escritos en C# (.NET).
- **Naturaleza del Artefacto**: Cuando un binario gestionado (.NET) se ejecuta, el motor CLR interactúa profundamente con el sistema operativo y registra telemetría de ejecución a nivel de runtime, independiente de la existencia física del archivo ejecutable en disco.

---

## 2. Ubicación y Métodos de Inspección en Screenshares

### A. Event Viewer — Application Log (`Application.evtx`)
- Ruta: `Windows Logs > Application`.
- **Filtro del Revisor**: `Source = ".NET Runtime"`.
- **Event IDs Críticos**:
  - `Event ID 1026`: Excepción no controlada (`Application Crash / Unhandled Exception`). Registra el nombre exacto del ensamblado (`Application: CheatLoader.exe`), la versión del framework y la pila de llamadas (Stack Trace) con nombres de métodos y clases internas.
  - `Event ID 1023`: Fallos de inicialización del runtime.

### B. Event Viewer — DotNETRuntime Log
- Ruta: `Applications and Services Logs > Microsoft > Windows > .NET-Runtime`.
- Registra eventos de bajo nivel del motor de ejecución: compilación JIT (Just-In-Time), carga de assemblies en dominios de aplicación (`AppDomain`) y eventos de recolección de basura.

### C. Fusion Log (Assembly Binding Log)
- Clave de Registro: `HKLM\SOFTWARE\Microsoft\Fusion`.
- Si el registro de enlace de ensamblados está habilitado (`EnableLog = 1` o `LogFailures = 1`), Windows registra cada intento de resolución de DLLs gestionadas. Si un cheat intentó cargar una dependencia y falló, la ruta completa del ensamblado eliminado queda grabada en el log de Fusion.

### D. Directorio UsageLogs de CLR
- Ruta: `%LOCALAPPDATA%\Microsoft\CLR_v4.0\UsageLogs\` (o `CLR_v4.0_32`).
- Contiene archivos de registro `.log` nombrados con el ejecutable que utilizó el runtime .NET (e.g., `loader.exe.log`).
- Registra qué bibliotecas estándar del framework fueron importadas por el binario.

### E. Correlación con Prefetch y AmCache
- **Prefetch (`*.pf`)**: Al inspeccionar el prefetch con `PECmd.exe`, la lista de archivos cargados por el proceso incluye `mscoree.dll`, `clr.dll`, `clrjit.dll` o `coreclr.dll`, confirmando de forma categórica que el programa era una aplicación .NET.
- **AmCache (`Amcache.hve`)**: Registra el hash SHA-1 del ensamblado gestionado.
