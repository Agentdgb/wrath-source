# Bypass Limpio: Evasión de Kernel-PnP, setupapi.dev.log y Servicios

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este estándar garantiza que el cliente no dependa de controladores de kernel sospechosos ni altere servicios del sistema operativo, logrando un perfil forense idéntico al de un sistema limpio.

---

## 1. Vector Principal: Arquitectura Userland Pura (Zero-Driver)
*Regla de diseño de Wrath / VANTA:*

1. **Cero Controladores de Kernel (`.sys`)**:
   - Wrath / VANTA opera íntegramente en modo usuario (Ring 3) mediante APIs nativas de Windows (`NTAPI` directa).
   - **Resultado en Visor de Eventos**:
     - Cero eventos en `Microsoft-Windows-Kernel-PnP` (cero Eventos 410, 411, 420, 430, 441).
     - Cero controladores no firmados o certificados vulnerables en la pestaña *Drivers* de System Informer.
     - Cero llamadas a `ZwLoadDriver` o mappers tipo `kdmapper` que dejen rastro en `PiDDBCacheTable` o `KernelHashBucketList`.

---

## 2. Inmunidad en `setupapi.dev.log` mediante Montaje VHD `NON_PNP`
*Ubicación en el código:* `Bypasses/tools/setup_vhd_environment.ps1`.

1. **El Problema**:
   - Montar un disco virtual con utilidades estándar (`diskpart` clásico o herramientas de terceros) genera eventos de dispositivo Plug-and-Play que se registran en `setupapi.dev.log`.
2. **Solución Wrath**:
   - El montaje se realiza mediante la API nativa Win32 de `virtdisk.dll` (`AttachVirtualDisk`) utilizando el flag:
     ```cpp
     ATTACH_VIRTUAL_DISK_FLAG_NON_PNP (0x00000040) | ATTACH_VIRTUAL_DISK_FLAG_NO_LOCAL_HOST (0x00000008)
     ```
   - Al marcarse como `NON_PNP`:
     - El kernel de Windows **NO emite eventos de instalación de hardware**.
     - No se añade ninguna entrada en `%SystemRoot%\INF\setupapi.dev.log`.
     - No se generan marcas de `Data Overwrite`.

---

## 3. Preservación Total de Servicios (`PcaSvc`, `DPS`, `DiagTrack`)
*Cumplimiento estricto de la Regla 1 del proyecto:*

1. **Servicios Permanentemente Activos**:
   - Ni `WrathLoader` ni las rutinas de teardown detienen o reinician `PcaSvc`, `DPS`, `EventLog` ni `SysMain`.
   - El tiempo de actividad (`uptime`) de todos los servicios coincide con `GetTickCount64` del sistema.
   - En `System.evtx` no existe ningún Evento 7036 para servicios de diagnóstico.
2. **Evasión de Telemetría sin Parar Servicios**:
   - La evasión de `PcaSvc` se logra no disparando las heurísticas de compatibilidad (loader mimetizado con manifiesto UAC moderno y cabeceras de compilación válidas).
   - La evasión del payload se logra inyectando directamente en `javaw.exe` vía manual mapping: `PcaSvc` solo monitorea la creación de procesos ejecutables principales, no los módulos mapeados internamente.

---

## 4. Hooks Confinados al Espacio de `javaw.exe`
*Ubicación en el código:* `wrath/client/` (Hooks JNI y MinHook).

1. Los hooks residen estrictamente dentro del espacio de memoria virtual de Minecraft Fabric (`javaw.exe`).
2. Jamás se tocan ni abren handles a procesos protegidos del sistema como `csrss.exe` o `lsass.exe`.
3. System Informer no detecta ningún hilo ajeno ni manipulación de vtables en procesos de Windows.
