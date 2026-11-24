# Detección Forense: Búsqueda Global por Expresiones Regulares en Registry Explorer

## 1. Identificación y Alcance de la Técnica
- **Herramienta**: `RegistryExplorer.exe` (Eric Zimmerman Tools) + `Timeline Explorer`.
- **Naturaleza**: Metodología de auditoría forense integral que carga simultáneamente todas las colmenas del registro de Windows (`NTUSER.DAT`, `SYSTEM`, `SOFTWARE`, `SAM`, `SECURITY`) e inspecciona sus valores mediante expresiones regulares (Regex).
- **Objetivo**: Extraer de forma masiva cualquier valor del registro que contenga rutas de archivos en el sistema de archivos para identificar persistencia, binarios sospechosos en carpetas temporales y rastros de ejecución.

---

## 2. Patrones de Expresiones Regulares Utilizados

### A. Regex Universal de Rutas de Sistema de Archivos
```regex
^([a-zA-Z]:\\.+)\\?$
```
- **Lógica**:
  - `[a-zA-Z]:\\`: Coincide con cualquier letra de unidad (`C:\`, `D:\`, `E:\`).
  - `.+`: Cadena de ruta completa.
  - `\\?`: Barra invertida final opcional.
- **Resultado**: Captura todas las claves y valores que contengan rutas absolutas en cualquier colmena del registro.

### B. Filtros Específicos de Inspección en Screenshares
1. **Solo Ejecutables**:
   ```regex
   ^([a-zA-Z]:\\.+\.exe)\\?$
   ```
   - Filtra exclusivamente rutas que terminen en `.exe`.
2. **Solo Bibliotecas Dinámicas (DLLs)**:
   ```regex
   ^([a-zA-Z]:\\.+\.dll)\\?$
   ```
   - Detecta bibliotecas vinculadas a inyecciones, AppInit_DLLs, o DLL Hijacking.
3. **Foco en Carpetas Críticas de Usuario**:
   ```regex
   ^C:\\Users\\.+\\AppData\\.+$
   ^C:\\Users\\Public\\.+
   ^C:\\Temp\\.+
   ```
   - Aísla ejecutables o librerías ubicadas en carpetas no estándar donde los programas legítimos rara vez instalan binarios independientes sin instalador formal.

---

## 3. Claves Forenses Analizadas tras la Búsqueda
Los resultados se exportan a Excel (`.xlsx`) y se analizan en Timeline Explorer clasificándolos en:

1. **Persistencia**:
   - `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run` / `RunOnce`
   - `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` / `RunOnce`
   - `HKLM\SYSTEM\CurrentControlSet\Services`
2. **Artefactos de Ejecución Reciente del Usuario**:
   - `BAM`: `SYSTEM\CurrentControlSet\Services\bam\State\UserSettings\<SID>`
   - `MuiCache`: `HKCR\Local Settings\Software\Microsoft\Windows\Shell\MuiCache` (almacena el nombre descriptivo y la ruta de cada ejecutable iniciado por el shell).
   - `AppCompatFlags\Compatibility Assistant\Store`: Rutas de ejecutables evaluados por PCA.
3. **Diálogos de Archivos**:
   - `OpenSavePidlMRU` y `LastVisitedPidlMRU` en `NTUSER.DAT`.
