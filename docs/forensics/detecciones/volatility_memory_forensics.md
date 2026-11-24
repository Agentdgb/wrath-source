# Detección Forense: Volatility Framework (Análisis de Memoria RAM)

## 1. Identificación y Alcance de la Plataforma
- **Plataforma**: Volatility Framework (Volatility 3 / Python 3.7+).
- **Naturaleza**: Framework forense líder a nivel mundial para el análisis profundo de volcados de memoria física (RAM) en incidentes de seguridad y screenshares.
- **Formato de Entrada**: Volcados `.raw`, `.dmp`, `.mem` adquiridos con FTK Imager, DumpIt, WinPmem o Magnet RAM Capture.
- **Objetivo**: Extracción de datos altamente volátiles (procesos activos y ocultos, descriptores VAD, inyecciones de código, handles de objetos del kernel, líneas de comando y árboles de procesos).

---

## 2. Taxonomía de Datos Volátiles
- **Non-Volatile Data (NVM)**: Datos permanentes que sobreviven al apagado sin corriente (colmenas de registro, USN Journal, $MFT, bases de datos ESE).
- **Low-Volatile Data**: Persisten tras el reinicio pero están sujetos a rotación automática o sobreescritura con el tiempo (Prefetch, eventos archivados).
- **Moderately Volatile Data**: Cachés temporales del sistema, entradas de resolución DNS, archivos de trabajo en `%TEMP%`.
- **Highly Volatile Data**: Información almacenada exclusivamente en la memoria RAM física activa (procesos en ejecución, conexiones de red establecidas, descriptores de memoria virtual VAD, tokens de seguridad, mutexes en memoria y buffers de consola).

---

## 3. Plugins Críticos de Volatility 3 en Screenshares

### A. `windows.malfind.Malfind` (Detección de Inyecciones de Memoria)
- **El plugin más temido en screenshares**:
  ```bash
  python vol.py -f memdump.mem windows.malfind.Malfind --pid <PID>
  ```
- **Mecanismo**:
  - Examina los Virtual Address Descriptors (VAD) de cada proceso buscando páginas de memoria con:
    1. Permisos ejecutables (`PAGE_EXECUTE_READWRITE` / `PAGE_EXECUTE_READ`).
    2. Tipo de memoria `MEM_PRIVATE` (memoria asignada dinámicamente sin respaldo de un archivo binario en disco).
    3. Cabeceras PE (`MZ` / `PE`) o instrucciones ensamblador sospechosas.
  - Vuelca y desensambla los primeros bytes del código inyectado mediante Capstone Engine.

### B. `windows.psscan.PsScan` y `windows.pstree.PsTree`
- **PsScan**: Escanea los pools de memoria del kernel (`NonPagedPool`) buscando estructuras `EPROCESS` huérfanas o no vinculadas en la lista doblemente enlazada `ActiveProcessLinks` (detecta procesos ocultos por rootkits vía DKOM).
- **PsTree**: Reconstruye la jerarquía padre-hijo (PID y PPID). Detecta anomalías como procesos de sistema ejecutándose con padres falsos.

### C. `windows.handles.Handles`
- Enumera todos los handles abiertos por un proceso (Handles a otros procesos con `PROCESS_ALL_ACCESS`, Mutexes nombrados, pipes y eventos).
- Detecta nombres de mutex de cheats públicos (`"MyCheatMutex"`, `"LoaderSync"`).

### D. `windows.cmdline.CmdLine`
- Extrae de la memoria del proceso los argumentos exactos pasados en la llamada de creación (`PEB->ProcessParameters->CommandLine`).
