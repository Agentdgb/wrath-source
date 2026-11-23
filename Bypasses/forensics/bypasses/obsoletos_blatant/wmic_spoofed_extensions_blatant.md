# Bypass Obsoleto / Blatant: Uso de WMIC y Extensiones Disfrazadas

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Técnicas infantiles ampliamente difundidas que generan banderas rojas evidentes e irrefutables en cualquier screenshare.

---

## 1. Ejecutar Cheats mediante `wmic process call create`
- **Técnica errónea**:
  ```cmd
  wmic process call create "C:\Users\Admin\Desktop\cheat.exe"
  ```
  Con la creencia de que WMI ejecuta el proceso "de forma invisible".
- **Por qué falla (Baneo Inmediato)**:
  - Los buffers de memoria de `Explorer.exe`, `SgrmBroker.exe` y `MsMpEng.exe` (Windows Defender) capturan la línea de comando exacta.
  - Al extraer cadenas de `Explorer.exe` o `MsMpEng.exe` y buscar `"wmic"`, el revisor lee:
    `"wmic process call create C:\Users\Admin\Desktop\cheat.exe"`
  - El revisor obtiene el nombre completo del ejecutable, la ruta y la confirmación de la técnica utilizada en una sola línea de texto.

---

## 2. Renombrar Ejecutables a `.txt`, `.png`, `.tmp` o `.dat` (Spoofed Extensions)
- **Técnica errónea**:
  Renombrar `cheat.exe` a `image.png`, `notes.txt` o `mydata.tmp` para "engañar a los escáneres".
- **Por qué falla (Detección al 100%)**:
  1. **Prefetch Anomaly**:
     - El servicio `SysMain` genera `IMAGE.PNG-XXXXXXXX.pf` o `NOTES.TXT-XXXXXXXX.pf`.
     - PECmd o WinPrefetchView detectan archivos prefetch que no terminan en `.exe`. Cualquier prefetch con extensión no estándar es baneo automático inmediato por encubrimiento de ejecutable.
  2. **Búsqueda en Everything**:
     - El revisor abre Everything y busca:
       `"This program cannot be run in DOS mode."`
     - Everything indexa el contenido de archivos en milisegundos. El archivo `image.png` salta en la primera posición porque contiene la cabecera DOS de un ejecutable PE de Windows.
  3. **Discrepancia en Detect It Easy (DiE)**:
     - DiE lee el magic number `0x5A4D` (`MZ`). Si la extensión es de imagen o texto pero el formato es PE32+, se marca como **Mismatched / Disguised PE**.
  4. **BAM y Event Logs**:
     - El registro `BAM` (`Services\bam\State\UserSettings\<SID>`) guarda la ruta completa: `\Device\HarddiskVolume3\...\image.png` como un binario ejecutado.
