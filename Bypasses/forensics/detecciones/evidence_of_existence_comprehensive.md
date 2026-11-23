# Detección Forense: Correlación Integral de Evidencia de Existencia

## 1. Identificación y Alcance del Procedimiento
- **Concepto**: "Evidence of Existence" (Evidencia de Existencia).
- **Principio Fundamental**: En Windows, un archivo eliminado **casi nunca desaparece por completo**. El sistema operativo genera una red entrelazada de metadatos en múltiples subsistemas independientes.
- **Objetivo de los Analistas**: Cruzar y correlacionar al menos 3 o 4 fuentes forenses distintas para demostrar que un ejecutable de trampa existió en el equipo, cuándo fue accedido y cuándo fue destruido.

---

## 2. Matriz de Artefactos de Existencia y Herramientas

| Artefacto | Ubicación | Qué Registra | Herramienta |
| :--- | :--- | :--- | :--- |
| **Recycle Bin** | `C:\$Recycle.Bin\<SID>` | `$I` (metadatos: ruta original, tamaño, fecha de borrado) y `$R` (contenido real del archivo). | `RBCmd.exe`, FTK Imager |
| **LNK Files** | `%APPDATA%\Microsoft\Windows\Recent\` | Accesos directos con ruta original, fecha del archivo, serial del volumen. | `LECmd.exe` |
| **Jump Lists** | `Recent\AutomaticDestinations\` | Archivos abiertos por aplicaciones ancladas en la barra de tareas. | `JLECmd.exe` |
| **Shellbags** | `UsrClass.dat` y `NTUSER.DAT` (`BagMRU` / `Bags`) | Historial y preferencias de cada carpeta navegada en Windows Explorer (incluso borradas). | `ShellBagsExplorer.exe` |
| **Open/Save MRU** | `Software\Microsoft\...\ComDlg32` | Historial de archivos abiertos/guardados en cuadros de diálogo estándar. | `RegistryExplorer.exe` |
| **RecentDocs** | `Software\Microsoft\...\RecentDocs` | Documentos y archivos accedidos recientemente ordenados por extensión. | `RegistryExplorer.exe` |
| **Prefetch** | `C:\Windows\Prefetch\*.pf` | Nombre del binario, RunCount, hasta 8 marcas temporales y DLLs mapeadas. | `PECmd.exe`, WinPrefetchView |
| **AmCache** | `C:\Windows\AppCompat\Programs\Amcache.hve` | Rutas completas, hashes criptográficos SHA-1, tamaño y primer uso. | `AmcacheParser.exe` |
| **ShimCache** | `SYSTEM\...\AppCompatCache` | Rutas completas y timestamps de binarios evaluados por el sistema de compatibilidad. | `AppCompatCacheParser.exe` |
| **Browser History** | `%USERPROFILE%\AppData\Local\...\History\` | Acceso a archivos locales mediante contexto de navegador (`file:///C:/...`). | SQLite Parsers, BrowsingHistoryView |
| **Windows Search** | `C:\ProgramData\Microsoft\Search\Data\...\Windows.edb` | Base de datos Jet Blue del servicio `WSearch`: indexación de nombres, rutas y contenido. | ESEDatabaseView |
| **Activity History** | `%LOCALAPPDATA%\ConnectedDevicesPlatform\...\ActivitiesCache.db` | Base de datos SQLite del Windows Timeline: aplicaciones abiertas y tiempo de uso activo. | SQLite Browser |

---

## 3. Metodología de Correlación Forense
Un analista profesional no depende de un solo artefacto. Reconstruye la cadena de custodia mediante triangulación:
1. **Descarga**: Localizada en el historial del navegador (`file:///` o download URL) y en el ADS `:Zone.Identifier`.
2. **Existencia**: Confirmada por `AmCache` (hash SHA-1) y `ShimCache` (presencia del PE).
3. **Ejecución**: Confirmada por `Prefetch` (RunCount incrementado), `BAM` y `UserAssist`.
4. **Interacción del Usuario**: Confirmada por `LNK Files`, `Jump Lists` y `Shellbags`.
5. **Borrado**: Confirmado por entradas `$I` en `Recycle Bin`, registros de borrado en `$LogFile` y entradas inactivas en el índice `$i30` de la carpeta padre.
