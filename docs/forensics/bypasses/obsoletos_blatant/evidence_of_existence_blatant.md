# Bypass Obsoleto / Blatant: Borrado Incompleto de Artefactos de Existencia

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Los errores de concepto más comunes que fracasan ante el análisis de correlación forense multi-artefacto.

---

## 1. Mover el Cheat a la Papelera de Reciclaje y Vaciarla
- **Técnica errónea**: Enviar `loader.exe` a la Papelera de Reciclaje y luego hacer clic en "Vaciar papelera".
- **Por qué falla ante RBCmd / FTK Imager**:
  - Al enviar un archivo a la papelera, Windows renombra el archivo como `$RXXXXXX.<ext>` (con el contenido original) y crea un archivo de metadatos `$IXXXXXX.<ext>`.
  - El archivo `$I` contiene en texto plano:
    - La ruta original del archivo (`C:\Users\Admin\Desktop\loader.exe`).
    - El tamaño exacto en bytes.
    - La marca de tiempo precisa en que fue enviado a la papelera.
  - Aunque el usuario "vacíe" la papelera, las herramientas forenses (`RBCmd`, `FTK Imager`) **carvean los archivos `$I` y `$R` desasignados de `C:\$Recycle.Bin\<SID>`**.
  - Además, el USN Journal y el `$LogFile` registraron la transacción de cambio de nombre y eliminación.

---

## 2. Configurar `NukeOnDelete = 1` en el Registro
- **Técnica errónea**:
  Modificar la clave de registro de la papelera:
  `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\BitBucket` -> `NukeOnDelete = 1` (para que los archivos se borren directamente sin ir a la papelera).
- **Por qué falla**:
  - Los scripts de inspección de Echo y Paladin verifican el valor de `NukeOnDelete`.
  - Un equipo con `NukeOnDelete = 1` activo levanta una bandera roja inmediata de encubrimiento premeditado (*Anti-Forensic Recycle Bin Configuration*).

---

## 3. Limpiar solo 1 o 2 Artefactos Dejando el Resto Intacto (Asincronía Forense)
- **Técnica errónea**:
  Borrar únicamente el archivo `.pf` de Prefetch o el historial de navegación, dejando los LNK files, Jump Lists, AmCache y Shellbags intactos.
- **Por qué falla**:
  - Es el "Principio de Intercambio de Locard":
    Si un archivo se ejecutó, dejó rastros en 6 lugares distintos.
  - Si el analista encuentra que el archivo no está en Prefetch pero figura con `Executed = True` en AmCache, tiene un acceso directo en `Recent\*.lnk` y una entrada en `ActivitiesCache.db`, la ausencia del archivo en Prefetch **demuestra que el Prefetch fue manipulado deliberadamente**, confirmando el uso de trampas y el intento de encubrimiento.

---

## 4. Corromper o Resetear `Windows.edb` o `ActivitiesCache.db`
- **Técnica errónea**: Forzar el borrado de la base de datos de Windows Search (`Windows.edb`) o la base SQLite del Timeline (`ActivitiesCache.db`).
- **Por qué falla**:
  - Se generan errores de inicialización de base de datos en los logs de eventos del sistema.
  - Una base de datos `Windows.edb` de 1 MB o un Timeline vacío en un usuario activo levanta una alerta directa de **Wiped Activity Database**.
