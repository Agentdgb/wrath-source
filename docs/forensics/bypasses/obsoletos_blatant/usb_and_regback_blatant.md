# Bypass Obsoleto / Blatant: El Mito del USB Plug & Pull y Limpiadores USB

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Una de las técnicas más antiguas y peores concebidas en la historia de los cheats, responsable de miles de baneos inmediatos en screenshares.

---

## 1. El Mito del USB "Plug and Pull" (Cargar desde Pendrive y Desconectarlo)
- **Técnica errónea**:
  Guardar el ejecutable del cheat en un pendrive USB (e.g. `E:\loader.exe`), conectarlo al PC, ejecutarlo para inyectar en Minecraft y desconectar el pendrive de inmediato antes de entrar a jugar o cuando piden screenshare.
- **Por qué falla ante los revisores (Baneo Inmediato)**:
  1. **Marcas Temporales en `Properties\0067`**:
     - Windows 8, 10 y 11 registran el instante exacto de extracción del USB en:
       `HKLM\SYSTEM\CurrentControlSet\Enum\USBSTOR\...\Properties\{GUID}\0067`
     - El revisor ve que un pendrive USB Kingston/SanDisk con serial `123456789ABC` fue desconectado a las 18:32:15 (exactamente 3 minutos antes de entrar a la llamada de AnyDesk).
  2. **Rutas en LNK y JumpLists**:
     - Si el usuario abrió el ejecutable desde el pendrive, en `%APPDATA%\...\Recent\` queda un archivo `.lnk` que apunta a `E:\loader.exe`, con el número de serie de volumen del pendrive.
  3. **Requerimiento Obligatorio de Conexión**:
     - El revisor exige: *"Vuelve a conectar la memoria USB con serial 123456789ABC que desconectaste hace 5 minutos"*.
     - Si el usuario se niega, alega que no tiene un USB o lo formateó: **Baneo Permanente Automático por Destrucción/Ocultamiento de Hardware de Trampas**.

---

## 2. Uso de Scripts o Programas "USB Cleaner"
- **Técnica errónea**: Ejecutar programas que prometen "borrar el historial de USBs" del registro de Windows.
- **Por qué falla**:
  - Para borrar `USBSTOR`, los programas requieren permisos SYSTEM forzados.
  - Al borrar las claves de registro:
    - `RegistryExplorer` recupera los nombres y seriales de los pendrives desde el espacio no asignado (slack space).
    - Desincronizan el registro con `%SystemRoot%\INF\setupapi.dev.log` (donde queda grabada la instalación del driver del pendrive con fecha y hora).
    - La contradicción entre `setupapi.dev.log` y un `USBSTOR` vacío es prueba directa de manipulación forense.

---

## 3. Borrar la Carpeta `RegBack`
- **Técnica errónea**: Borrar los archivos de respaldo en `C:\Windows\System32\config\RegBack\` creyendo que así se eliminan copias del registro.
- **Por qué falla**:
  - Despierta alertas críticas de integridad en cualquier script automatizado de auditoría de Windows.
