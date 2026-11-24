# Bypass Obsoleto / Blatant: Vaciado Masivo de MRU y Jump Lists

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Técnicas destructivas basadas en scripts "cleaner" públicos que generan baneo inmediato al ejecutar `Analyze-MRURegistry.ps1` o `JumpListAnalysis.ps1`.

---

## 1. Borrado Completo de la Clave `RunMRU`
- **Técnica errónea**:
  ```cmd
  reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\RunMRU" /f
  ```
- **Por qué falla ante Analyze-MRURegistry.ps1**:
  - Todo usuario normal de Windows tiene entradas en `RunMRU` acumuladas durante semanas o meses (`dxdiag`, `cmd`, `taskmgr`, `appdata`, etc.).
  - Cuando el analista ejecuta el script de PowerShell-Hunter, si la clave no existe o devuelve 0 resultados, el script emite una alerta roja:
    **`[ALERT] RunMRU Key Deleted / Empty - Recent Activity Purged`**.
  - Este es uno de los indicadores más evidentes de intento de encubrimiento rápido.

---

## 2. Purgado de Carpetas de Jump Lists
- **Técnica errónea**:
  ```cmd
  del /f /q %APPDATA%\Microsoft\Windows\Recent\AutomaticDestinations\*
  del /f /q %APPDATA%\Microsoft\Windows\Recent\CustomDestinations\*
  ```
- **Por qué falla ante JumpListAnalysis.ps1**:
  - Elimina de un solo golpe el historial de todas las aplicaciones de la barra de tareas de Windows (el usuario pierde el historial de pestañas recientes de navegadores, documentos de Word/Excel y carpetas ancladas).
  - Al ejecutar `JumpListAnalysis.ps1`, el analista inspecciona la fecha de creación de los archivos `.automaticDestinations-ms`.
  - Si la carpeta está vacía o todos los archivos fueron creados hace 15 minutos en un equipo con Windows encendido hace días, se concluye **manipulación y purga activa de evidencias forenses**.

---

## 3. Deshabilitar el Historial de Archivos Recientes en las Opciones del Explorador
- **Técnica errónea**: Desmarcar "Mostrar archivos usados recientemente en el Acceso rápido" en la configuración del Explorador de Windows.
- **Por qué falla**:
  - Los scripts de screenshare verifican el valor de la clave de registro `ShowRecent` y `ShowFrequent`.
  - Si ambas opciones están forzadamente desactivadas, los analistas de screenshare exigen encenderlas o marcan al usuario con bandera de sospecha de bypass previo, procediendo a un escaneo profundo de memoria y MFT.
