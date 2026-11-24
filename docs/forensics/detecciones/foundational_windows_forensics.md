# Detección Forense: Fundamentos del Sistema Windows, Colmenas y Artefactos USB

## 1. Identificación y Alcance de la Metodología
- **Componentes Principales**: Arquitectura de colmenas del Registro (`SYSTEM`, `SOFTWARE`, `NTUSER.DAT`, `UsrClass.dat`), copias de seguridad (`RegBack`), artefactos de conexión de dispositivos USB (`USBSTOR`, `MountPoints2`, `EMDMgmt`) y perfiles de red (NLA).
- **Propósito Forense**: Reconstrucción cronológica de hardware externo conectado al equipo, carpetas abiertas y configuración de red.

---

## 2. Artefactos del Registro y Respaldos (`RegBack`)
1. **Ubicación de Colmenas Principales**:
   - `%SystemRoot%\System32\config\`: `SYSTEM`, `SOFTWARE`, `SECURITY`, `SAM`, `DEFAULT`.
   - Por usuario: `%USERPROFILE%\NTUSER.DAT` (HKCU) y `%LOCALAPPDATA%\Microsoft\Windows\UsrClass.dat`.
2. **Carpeta de Respaldos `RegBack`**:
   - `%SystemRoot%\System32\config\RegBack\`: Contiene copias de respaldo de las colmenas. Si un usuario elimina claves activas o limpia el registro, los analistas comparan el registro en vivo contra los respaldos de `RegBack` para recuperar claves previas al borrado.
3. **Control Sets (`CurrentControlSet` vs `ControlSet001`)**:
   - En sistemas vivos se visualiza `CurrentControlSet`. En imágenes forenses o análisis offline se analiza `ControlSet001` (indicado en `HKLM\SYSTEM\Select\Current`).

---

## 3. Forense de Dispositivos USB (USB Device Forensics)
Cuando se conecta una memoria USB o pendrive a un equipo con Windows, el sistema graba una cadena persistente e imborrable en múltiples claves del registro y archivos de log:

1. **Identificación del Dispositivo**:
   - `HKLM\SYSTEM\CurrentControlSet\Enum\USBSTOR`: Registra Fabricante, Modelo, Versión de Firmware y Número de Serie del pendrive.
   - `HKLM\SYSTEM\CurrentControlSet\Enum\USB`: Vendor ID (VID) y Product ID (PID).
2. **Marcas de Tiempo de Conexión y Desconexión (Windows 8 / 10 / 11)**:
   - Claves bajo `Enum\USBSTOR\<Device>\Properties\{GUID}`:
     - **`0064`**: Fecha y hora de la **primera instalación** del dispositivo en el equipo.
     - **`0066`**: Fecha y hora de la **última conexión**.
     - **`0067`**: Fecha y hora de la **última desconexión / extracción** (`Last Removal Time`).
   - `setupapi.dev.log`: Registra el primer evento de conexión de hardware.
3. **Asignación de Usuario y Unidad**:
   - `HKLM\SYSTEM\MountedDevices`: Letra de unidad asignada (`E:`, `F:`) y GUID del volumen.
   - `NTUSER.DAT\Software\Microsoft\Windows\CurrentVersion\Explorer\MountPoints2`: Asocia el dispositivo USB con el usuario exacto que inició sesión y lo montó.
   - `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\EMDMgmt`: Conserva el historial de números de serie de volúmenes externos incluso si el pendrive fue formateado.

---

## 4. Network Location Awareness (NLA)
- Ruta: `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\NetworkList`
  - `Signatures\Unmanaged`: Registra la MAC de la puerta de enlace (`DefaultGatewayMac`), el SSID de la red Wi-Fi y el GUID del perfil.
  - `Profiles`: Registra `DateCreated` y `DateLastConnected` en formato de 128 bits de Windows, demostrando en qué redes físicas estuvo conectado el equipo.
