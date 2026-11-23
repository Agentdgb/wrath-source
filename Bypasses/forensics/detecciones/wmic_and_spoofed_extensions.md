# Detección Forense: Ejecución vía WMIC y Extensiones Disfrazadas (Spoofed Extensions)

## 1. Identificación y Alcance de las Técnicas
Este análisis cubre dos vectores comunes de ejecución encubierta y sus firmas forenses:
1. **Ejecución vía WMI (`wmic process call create`)** y su persistencia en buffers de memoria de procesos del sistema.
2. **Extensiones Disfrazadas (Spoofed Extensions)**: Renombrado de archivos ejecutables PE a extensiones inocuas (`.txt`, `.png`, `.tmp`, `.dat`, o sin extensión).

---

## 2. Detección de Invocación WMIC en Procesos del Sistema
Cuando un usuario ejecuta un binario mediante WMI:
```cmd
wmic process call create "C:\ruta\cheat.exe"
```
La línea de comandos no solo se procesa por el subsistema WMI, sino que sus cadenas persisten en la memoria virtual de tres procesos críticos de Windows:

1. **`Explorer.exe`**:
   - Si se lanzó desde el shell, menú ejecutar o scripts de usuario:
   - Al extraer cadenas de memoria de `Explorer.exe` (usando Process Hacker, System Informer o ProcDump) y buscar `"wmic"`:
     - Devuelve la línea completa: `"wmic process call create nombredelcheat.exe"`, exponiendo el nombre exacto del ejecutable y la técnica utilizada.
2. **`SgrmBroker.exe` (System Guard Runtime Monitor Broker)**:
   - Componente de seguridad del kernel de Windows que supervisa la integridad de la plataforma.
   - Retiene la cadena de invocación `"wmic"` en sus buffers de telemetría de monitoreo de procesos.
3. **`MsMpEng.exe` (Microsoft Defender Antivirus Service)**:
   - Si el antivirus de Windows está activo, su motor de inspección de comportamiento intercepta y almacena en memoria la llamada de creación de WMI junto con el nombre completo y la ruta del binario analizado.

---

## 3. Detección de Extensiones Disfrazadas (Spoofed Extensions)
Los usuarios intentan eludir escáneres estáticos renombrando `cheat.exe` a `notes.txt`, `logo.png` o `mydata.tmp`, y ejecutándolo con `wmic process call create "notes.txt"` o `Start-Process`.

### Métodos de Detección Forense:
1. **Anomalía en Prefetch (`C:\Windows\Prefetch\`)**:
   - El servicio `SysMain` genera el archivo Prefetch basándose en el nombre real con el que se ejecutó el proceso.
   - En la carpeta de Prefetch aparece un archivo con nombre anómalo:
     `NOTES.TXT-F1A2B3C4.pf` o `LOGO.PNG-8A7B6C5D.pf`.
   - **Flag Crítico**: Un archivo en Prefetch con extensión distinta a `.exe` es una prueba irrefutable del 100% de que se ejecutó un archivo PE camuflado.
2. **Análisis de Memoria de Servicios (`csrss.exe`, `dps`)**:
   - Los analistas filtran los nombres de ejecutables cargados en memoria mediante expresiones regulares como:
     ```regex
     ^!![A-Z]((?!Exe).)$
     ```
     para aislar ejecutables que no terminen en `.exe`.
3. **Inspección de Contenido y Cabeceras PE**:
   - **Búsqueda en Everything**: Búsqueda del string `"This program cannot be run in DOS mode."`:
     - Everything indexa el contenido de archivos. Si un archivo `.png` o `.txt` contiene esta cadena en sus primeros 512 bytes, se identifica de inmediato como un binario PE ejecutable disfrazado.
   - **Detect It Easy (DiE) y Signature Checkers**:
     - DiE analiza el "Magic Number" del archivo (`0x5A4D` / `MZ`). Si la extensión es `.png` pero la cabecera es PE/COFF, salta la alerta de discrepancia de tipo de archivo (*Mismatched File Type / PE Masquerading*).
4. **Registros de Ejecución del Sistema**:
   - `BAM` (`Services\bam\State\UserSettings\<SID>`) y `ActivitiesCache.db` registran la ruta completa con el nombre disfrazado (`notes.txt`) asociada a eventos de ejecución.
