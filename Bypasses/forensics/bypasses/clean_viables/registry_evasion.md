# Bypass Limpio: Evasión de RegistryExplorer, rla y Slack Space del Registro

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass neutraliza la recuperación forense en RegistryExplorer y rla sin detener servicios y sin dejar valores huérfanos en celdas desasignadas ("Associated deleted records").

---

## 1. Técnica Anti-Slack Space en Registro (Implementación Wrath `bam.cpp`)
*Ubicación en el código:* `Bypasses/trace/bam.cpp` (`delete_value_anti_slack`).

Cuando es necesario retirar una entrada de ejecución en `SYSTEM\...\Services\bam\State\UserSettings\<SID>`:
1. **El Problema**: `RegDeleteValueW()` solo desvincula el puntero de la celda `vk`. Los caracteres del nombre del ejecutable quedan intactos en el espacio no asignado (slack space) del bloque `hbin`, donde RegistryExplorer los recupera de inmediato.
2. **Solución Atómica Wrath**:
   - **Paso 1 — Sobrescritura de Contenido**: Antes de eliminar el valor, se sobreescribe el contenido de la celda existente con los bytes de un binario nativo del sistema legítimo (e.g. `\Device\HarddiskVolume3\Windows\System32\svchost.exe`).
   - **Paso 2 — Flush de Transacción**: Se fuerza `RegFlushKey(hKey)` para sincronizar la memoria con los transaction logs (`.LOG1` / `.LOG2`), desplazando cualquier residuo previo.
   - **Paso 3 — Asignación de Señuelo**: Se crea una entrada legítima del binario nativo.
   - **Paso 4 — Desvinculación Limpia**: Se elimina el valor original (`RegDeleteValueW`). Al inspeccionar el slack space con RegistryExplorer, los bytes recuperados solo contienen el señuelo legítimo de `svchost.exe`.

---

## 2. Prevención Total en ComDlg32 y Explorer Artifacts
*Diseño del Loader Wrath:*

1. **Cero uso de Diálogos Estándar**:
   - `WrathLoader.exe` jamás invoca `GetOpenFileName` ni `GetSaveFileName` de `comdlg32.dll`.
   - Las rutas de configuración, inyección y assets se resuelven en memoria o mediante parámetros relativos/cifrados.
   - **Resultado**: `ComDlg32\OpenSavePidlMRU` y `LastVisitedPidlMRU` en `NTUSER.DAT` **no contienen una sola entrada** relacionada con Wrath. No hay nada que borrar porque nada se registró.
2. **Cero Artefactos UserAssist**:
   - `WrathLoader.exe` se ejecuta directamente o como proceso secundario sin invocar el shell gráfico de Windows Explorer (`ShellExecuteEx` innecesario / atajos `.lnk`), impidiendo la generación de registros en `UserAssist\{GUID}\Count`.

---

## 3. Aislamiento por Montaje VHD (`NON_PNP` / Sin Letra de Unidad)
*Ubicación en el código:* `Bypasses/tools/setup_vhd_environment.ps1`.

1. **Evasión de `MountedDevices`**:
   - Al montar el contenedor VHD con los flags `ATTACH_VIRTUAL_DISK_FLAG_NON_PNP (0x40)` y `ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER (0x02)`, y enlazarlo directamente a un NTFS Volume Mount Point:
   - Windows **no registra** un nuevo dispositivo SCSI/disco en `SYSTEM\MountedDevices`.
   - No se registran letras de unidad (`D:`, `E:`) asociadas al contenedor temporal.

---

## 4. Preservación de `LastWriteTime` en Claves Críticas
1. Al interactuar con claves como BAM o PCA, se captura el `LastWriteTime` previo mediante `RegQueryInfoKeyW`.
2. Tras la mitigación anti-slack, se utiliza `NtSetInformationKey` con la clase `KeySetInformationClass` para restaurar la marca de tiempo original de la clave `nk`, evitando que los inspectores detecten que la clave fue modificada minutos antes del screenshare.
