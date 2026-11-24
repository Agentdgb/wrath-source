# Bypass Limpio: Integridad Absoluta de Logs y USN Journal (Zero-Wipe)

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este estándar descarta por completo el borrado destructivo de logs o diarios de cambios. La arquitectura de Wrath garantiza la ausencia de trazas mediante aislamiento de capas, manteniendo la integridad forense del sistema al 100%.

---

## 1. Filosofía "Zero-Wipe": Cero Manipulación de Logs de Eventos
*Regla de diseño de Wrath / VANTA:*

1. **Cero Invocación de Herramientas de Limpieza**:
   - Wrath jamás ejecuta `wevtutil cl`, `wevtutil sl` ni `Clear-EventLog`.
   - Cero Evento 1102 en `Security.evtx` ("The audit log was cleared").
   - Cero Evento 104 en `System.evtx`.
   - Al abrir el Visor de Eventos o analizar con Hayabusa, los registros de eventos de Windows presentan una continuidad temporal natural, sin huecos ni alertas críticas de borrado de auditoría.

---

## 2. Preservación del USN Journal (`fsutil usn queryjournal C:`)
*Ubicación en el código:* `Bypasses/tools/setup_vhd_environment.ps1`.

1. **Nunca Vaciar el Diario**:
   - Wrath **nunca ejecuta `fsutil usn deletejournal`**.
2. **Aislamiento por VHD Volátil**:
   - Las operaciones de despliegue, inyección y guardado de perfiles ocurren dentro del sistema de archivos interno del VHD (`Backup_2026.vhd`).
   - Cada sistema de archivos NTFS mantiene su propio `$UsnJrnl` independiente.
   - Al ejecutar `fsutil usn queryjournal C:`, el USN Journal del disco host `C:\` muestra un flujo ininterrumpido de transacciones normales del sistema operativo, sin transacciones de ejecutables de cheats ni saltos bruscos de IDs.

---

## 3. Cero Instalación de Servicios (Sin Event ID 4697)
1. Wrath opera íntegramente en Userland sin drivers `.sys`.
2. No se ejecutan llamadas a `CreateServiceW` ni `sc create ... type= kernel`.
3. **Resultado en `Security.evtx`**: En el log de seguridad no existe ningún **Event ID 4697** ("A service was installed in the system"), eliminando la principal firma de instalación de drivers de cheats.

---

## 4. Inmunidad en `setupapi.dev.log`
1. Al montar el contenedor VHD mediante la API Win32 `virtdisk.dll` con los flags:
   ```cpp
   ATTACH_VIRTUAL_DISK_FLAG_NON_PNP (0x40) | ATTACH_VIRTUAL_DISK_FLAG_NO_LOCAL_HOST (0x08)
   ```
2. Windows no genera eventos de Plug-and-Play.
3. No se añade ninguna entrada en `%SystemRoot%\INF\setupapi.dev.log` ni en `SetupAPI\Dev.log`. Cero menciones de `Data Overwrite`.

---

## 5. Claves de Registro de EventLog Intactas
1. `HKLM\SYSTEM\CurrentControlSet\Services\EventLog` permanece completamente intacto:
   - Cero cambios en valores `File`, `MaxSize`, `Retention` o `Start`.
   - Cero eventos generados en Sysmon (Event ID 12 y 13).
