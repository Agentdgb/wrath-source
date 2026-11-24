# Detección Forense: Evidencia de Ejecución y Existencia de Archivos

## 1. Identificación y Alcance de los Artefactos
En auditorías de screenshare y revisiones forenses en vivo, los analistas cruzan dos familias principales de artefactos de Windows:

---

## 2. Artefactos de Evidencia de Ejecución (Program Execution)

### A. Prefetch (`C:\Windows\Prefetch\*.pf`)
- Administrado por el servicio `SysMain` (Superfetch). Diseñado para acelerar la carga de aplicaciones mapeando bloques de disco a memoria.
- **Metadatos Almacenados**:
  - Nombre del binario + Hash de 8 caracteres hexadecimales derivado de la ruta del ejecutable (e.g. `ROBOCOPY.EXE-F0F944E5.pf`).
  - Contador de ejecuciones (`RunCount`).
  - Hasta 8 timestamps de ejecuciones anteriores (en Windows 8/10/11).
  - Lista completa de archivos y bibliotecas DLL leídas/cargadas durante los primeros 10 segundos de vida del proceso.
- **Herramientas**: `PECmd.exe` (Eric Zimmerman), `WinPrefetchView`.

### B. ShimCache y AmCache
- `AppCompatCache` en `SYSTEM` y `Amcache.hve` en `AppCompat\Programs`.
- Prueban la existencia histórica de binarios, hashes SHA-1 y compatibilidad (ver módulos específicos correspondientes).

### C. UserAssist
- Ruta: `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\UserAssist\{GUID}\Count`
- Almacena nombres de aplicaciones GUI ejecutadas a través de Windows Explorer, codificadas en **ROT13**.
- Registra contador de ejecuciones, tiempo acumulado en pantalla (`Focus Time`) y fecha de última ejecución.
- **Herramientas**: `RegistryExplorer.exe`.

### D. Event ID 4688 (Process Creation)
- Log de seguridad `Security.evtx`.
- Si el rastreo de procesos está habilitado, registra el nombre del proceso, usuario, proceso padre y argumentos de línea de comandos.
- **Herramientas**: `EvtxECmd.exe`.

---

## 3. Artefactos de Evidencia de Existencia de Archivos (File Existence)

### A. Archivos de Acceso Directo LNK (`%APPDATA%\Microsoft\Windows\Recent\*.lnk`)
- Creados automáticamente por Windows Explorer cada vez que un usuario abre o ejecuta un archivo local o remoto.
- **Metadatos**:
  - Ruta original absoluta del archivo de destino.
  - Marcas de tiempo de creación y modificación del archivo original.
  - Número de serie del volumen (`VolumeSerialNumber`), tamaño exacto y atributos.
- **Herramientas**: `LECmd.exe` (Eric Zimmerman).

### B. Jump Lists (`AutomaticDestinations` / `CustomDestinations`)
- Accesos directos y archivos recientes agrupados por Application ID en la barra de tareas.
- **Herramientas**: `JLECmd.exe`.

### C. Shellbags
- Rutas en el Registro:
  - `HKCU\Software\Microsoft\Windows\Shell\BagMRU` y `Bags`
  - `HKCU\Software\Classes\Local Settings\Software\Microsoft\Windows\Shell\BagMRU` y `Bags` (en `UsrClass.dat`)
- **Qué registran**:
  - Las preferencias de visualización (iconos grandes, detalles, posición de ventana) de **cada carpeta navegada por el usuario en Windows Explorer**.
  - Permite reconstruir carpetas que alguna vez existieron en discos locales, VHDs o memorias USB, **incluso después de que la carpeta haya sido completamente eliminada**.
- **Herramientas**: `ShellBagsExplorer.exe`.
