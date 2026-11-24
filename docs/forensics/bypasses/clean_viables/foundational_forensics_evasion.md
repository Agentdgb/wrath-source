# Bypass Limpio: Evasión de Artefactos Fundamentales y Dispositivos USB

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass asegura que el despliegue del cliente no dependa de dispositivos USB externos (eliminando el vector más fácil de detectar en screenshares) y no genere entradas en `USBSTOR`, `MountedDevices`, `MountPoints2` ni `RegBack`.

---

## 1. Vector Principal: Prohibición Total de Dispositivos USB Externos
*Regla operativa de Wrath / VANTA:*

1. **La Trampa del "Plug and Pull"**:
   - Muchos jugadores cargan el cheat desde un pendrive y lo desconectan antes del screenshare.
   - Como se documentó, Windows registra la desconexión con precisión de segundo en la clave `0067` de `USBSTOR`.
2. **Estándar Wrath**:
   - **NUNCA se utiliza una memoria USB o disco extraíble para cargar Wrath**.
   - Cero dispositivos USB conectados recientemente antes de la sesión de juego.
   - En `USBSTOR`, `MountedDevices` y `MountPoints2` **no existe ninguna entrada sospechosa** con marcas de tiempo recientes coincidentes con el inicio del juego.
   - El revisor inspecciona la clave `0067` y solo encuentra dispositivos USB legítimos de hardware (ratón, teclado, auriculares) o pendrives conectados semanas o meses atrás.

---

## 2. Inmunidad PnP en el Montaje VHD (`setup_vhd_environment.ps1`)
*Ubicación en el código:* `Bypasses/tools/setup_vhd_environment.ps1`.

1. **El Problema del Montaje Tradicional**:
   - Montar un disco virtual con utilidades estándar genera un nuevo volumen SCSI o PnP que deja entradas en `Enum\SCSI` y en `MountedDevices`.
2. **Solución Wrath**:
   - El script `setup_vhd_environment.ps1` invoca la API Win32 de `virtdisk.dll` (`AttachVirtualDisk`) utilizando:
     ```cpp
     ATTACH_VIRTUAL_DISK_FLAG_NON_PNP (0x00000040) |
     ATTACH_VIRTUAL_DISK_FLAG_NO_LOCAL_HOST (0x00000008) |
     ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER (0x00000002)
     ```
   - **Efecto Forense**:
     - Windows **no emite eventos PnP** en `setupapi.dev.log`.
     - No se registra en `USBSTOR` ni `MountedDevices`.
     - No se registra en `MountPoints2` de `NTUSER.DAT`.
     - La partición se enlaza a un Volume Mount Point interno, sin letra de unidad.

---

## 3. Preservación Estricta de Respaldos de Registro (`RegBack`)
1. Wrath no intenta manipular ni borrar la carpeta `%SystemRoot%\System32\config\RegBack\`.
2. Dado que Wrath no elimina masivamente claves del registro activo (utiliza anti-slack scrubbing en BAM y reversión atómica de snapshots), una comparación diferencial entre `SYSTEM` en vivo y el backup en `RegBack` arroja una sincronía perfecta sin claves de trampas eliminadas.

---

## 4. Estabilidad en Network Location Awareness (NLA)
1. Wrath no instala adaptadores de red TAP/TUN virtuales, ni proxies locales, ni VPNs efímeras.
2. Toda la comunicación de red de Minecraft viaja por el adaptador físico oficial registrado en `NetworkList\Profiles`.
3. Cero perfiles de red desconocidos o huérfanos.
