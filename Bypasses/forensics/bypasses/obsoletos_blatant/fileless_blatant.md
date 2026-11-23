# Bypass Obsoleto / Blatant: Borrado de PSReadLine, Vaciado de Logs y Deshabilitación de AMSI

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Métodos ampliamente difundidos en foros públicos de cheats que constituyen las mayores causas de baneo inmediato en screenshares forenses.

---

## 1. Borrar `ConsoleHost_history.txt`
- **Técnica errónea**:
  ```powershell
  Remove-Item (Get-PSReadLineOption).HistorySavePath -Force
  ```
- **Por qué falla ante los analistas (Baneo Inmediato)**:
  - Todo usuario de Windows con PowerShell activo tiene su archivo `ConsoleHost_history.txt`.
  - Cuando un revisor de screenshare busca el archivo y no existe o encuentra un archivo con fecha de creación de hace 5 minutos con 0 bytes, marca al usuario con **Tampering / Destrucción de Historial**.
  - **Fallo Crítico**: Borrar el archivo de texto **NO borra los registros de eventos de Windows**. El Event ID 4104 (ScriptBlock Logging) en `Microsoft-Windows-PowerShell%4Operational.evtx` ya registró el script completo con su timestamp y parámetros.

---

## 2. Vaciado de Event Logs (`wevtutil cl` / Event ID 1102)
- **Técnica errónea**:
  ```cmd
  wevtutil cl "Microsoft-Windows-PowerShell/Operational"
  wevtutil cl "Security"
  wevtutil cl "System"
  ```
- **Por qué falla ante Hayabusa y Event Viewer**:
  - Al vaciar el log de seguridad, Windows genera automáticamente el **Event ID 1102 ("The audit log was cleared")** por el usuario actual.
  - Al vaciar otros logs, se genera el **Event ID 104** en `System.evtx`.
  - Al ejecutar `Hayabusa`:
    ```cmd
    hayabusa.exe csv-timeline --output CSVOutput.csv -d C:\Windows\System32\winevt\Logs
    ```
    Hayabusa coloca el Evento 1102 / 104 en la parte superior como alerta **CRITICAL — Anti-Forensic Activity / Event Log Wiped**.
  - Es la prueba reina de que el usuario ejecutó una herramienta de trampa y borró las pruebas.

---

## 3. Deshabilitar AMSI en el Registro (`EnableAmsi = 0`)
- **Técnica errónea**:
  ```powershell
  Set-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows Script\Settings' -Name 'EnableAmsi' -Value 0
  ```
- **Por qué falla**:
  - Modifica la configuración global de seguridad de Windows.
  - Los scripts de detección de cheats en screenshares verifican directamente esta clave del registro. Si está en 0 o deshabilitada, se cataloga automáticamente como manipulación del sistema de seguridad.

---

## 4. Ofuscación Superficial (`Invoke-Obfuscation`)
- **Técnica errónea**: Usar concatenación de cadenas, ticks (`w`i`n`d`o`w`s`), codificación en base64 o cifrado simple para ejecutar scripts de cheats en PowerShell.
- **Por qué falla**:
  - **Script Block Logging (Event ID 4104)**: El motor de PowerShell desofusca el código antes de compilarlo y pasarlo al CLR. El Evento 4104 registra la representación de texto desofuscada y plana del script.
  - Hayabusa y los analizadores de logs detectan cadenas de alta entropía o llamadas a `[Convert]::FromBase64String` como comportamiento malicioso clasificado.
