# Bypass Limpio: Ejecución Sigilosa sin Intérpretes Scripting

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

La estrategia de Wrath / VANTA no se basa en trucos frágiles de ofuscación de scripts ni en scripts de PowerShell o CMD que dejan eventos en `winevt` y registros en PSReadLine, sino en **ejecución nativa pura C++20 / NTAPI**.

---

## 1. Vector Principal: Cero Invocación de PowerShell o CMD
*Regla de arquitectura de Wrath / VANTA:*

1. **Loader Nativo en C++20 Puro**:
   - `WrathLoader.exe` es un binario compilado nativamente que no delega operaciones a `powershell.exe`, `pwsh.exe`, `cmd.exe` ni `wscript.exe`.
   - **Resultado Inmune**:
     - No genera archivo `%APPDATA%\...\PSReadLine\ConsoleHost_history.txt`.
     - No genera Event ID 4104 (ScriptBlock Logging) ni Event ID 600 en `Microsoft-Windows-PowerShell%4Operational.evtx`.
     - `Hayabusa` no detecta ningún script sospechoso de PowerShell ni líneas de comandos anómalas en el análisis de logs de eventos.
     - AMSI para scripting no se invoca porque no hay código de script interpretado.

---

## 2. Higiene de PSReadLine en Herramientas de Mantenimiento (`setup_vhd_environment.ps1`)
Si para la configuración del entorno VHD se utiliza PowerShell:

1. **Nunca Borrar el Archivo `ConsoleHost_history.txt`**:
   - Borrar el archivo activa la alerta inmediata de *History Wiped*.
2. **Ejecución con Historial Desvinculado**:
   - Cuando se invoque PowerShell para montar el VHD o ejecutar pruebas, se ejecuta indicando:
     ```powershell
     powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Set-PSReadLineOption -HistorySaveStyle SaveNothing; & '.\Bypasses\tools\setup_vhd_environment.ps1'"
     ```
   - Al configurar `-HistorySaveStyle SaveNothing`, PSReadLine no escribe ninguna línea de esa sesión en `ConsoleHost_history.txt`.
   - El archivo de historial preexistente del usuario permanece con su contenido y fechas legítimas anteriores, sin registrar los comandos de Wrath y sin faltar en disco.

---

## 3. Cero Persistencia en Registro y Tareas Programadas
1. **Sin Entradas en `Run` / `RunOnce`**:
   - Wrath no es malware ni troyano que necesite persistir tras el reinicio. Es un cliente de inyección bajo demanda.
   - Las claves `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` y `HKLM\...\Run` permanecen inalteradas, superando cualquier auditoría de `Regshot`.
2. **Sin Tareas Programadas (`schtasks`)**:
   - Cero tareas creadas en el Programador de Tareas de Windows. La salida de `schtasks /query /fo LIST /v` permanece 100% limpia.

---

## 4. Parcheo Quirúrgico de AMSI en Memoria Propia (Sólo para Módulos Internos)
Si un componente interno en desarrollo requiriera inicializar CLR/.NET en memoria:
1. Se parchea la función `AmsiScanBuffer` en la memoria del propio proceso cargando `amsi.dll` y sobrescribiendo el prólogo de la función con un retorno limpio:
   ```cpp
   // x64: mov eax, 0x80070057 (E_INVALIDARG) o xor eax, eax (AMSI_RESULT_CLEAN); ret
   uint8_t patch[] = { 0xB8, 0x57, 0x00, 0x07, 0x80, 0xC3 };
   ```
2. **Sin Tocar el Registro**:
   - Jamás se modifica el valor del registro `EnableAmsi` en `HKLM`. El parche reside únicamente en el espacio de memoria privada del proceso y no altera la configuración del sistema operativo.
