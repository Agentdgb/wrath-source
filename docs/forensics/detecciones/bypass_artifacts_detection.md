# Detección Forense: Detección de Bypasses, Drivers y Manipulación de Servicios

## 1. Identificación y Alcance del Análisis
- **Objetivo**: Detección de herramientas de bypass activas o recientes diseñadas para encubrir trampas mediante manipulación de memoria, alteración de servicios críticos y uso de controladores de kernel.
- **Áreas Críticas de Auditoría Forense**:
  - Controladores en modo kernel (`.sys`) y eventos PnP.
  - Registros de instalación en `setupapi.dev.log`.
  - Manipulación y estado de salud de servicios (`PcaSvc`, `DPS`, `DiagTrack`).
  - Procesos de sistema (`csrss.exe`, `lsass.exe`).
- **Herramientas Utilizadas**:
  - `System Informer (Process Hacker)`: Pestaña *Drivers* y módulos en modo kernel.
  - `Event Viewer`: Logs de `Kernel-PnP`.
  - Parseadores de logs de texto (`setupapi.dev.log`) y Timeline Explorer.

---

## 2. Artefactos y Métodos de Detección

### A. Controladores de Kernel (Drivers) y Eventos Kernel-PnP
- Ruta del Visor de Eventos:
  `Application and Services Logs > Microsoft > Windows > Kernel-PnP > Configuration / Device Management`
- **Event IDs Clave**:
  - `Event ID 410`: Instalación exitosa de dispositivo/driver.
  - `Event ID 411`: Controlador cargado para un dispositivo.
  - `Event ID 420`: Proceso de inicio de instalación de driver.
  - `Event ID 430`: Fallo en la carga de driver (típico de mappers bloqueados por DSE o HVCI).
  - `Event ID 441`: Dispositivo/driver retirado o descargado del sistema.
- **Firmas en Screenshare**: Si un usuario utilizó un mapper de kernel (e.g., `kdmapper`, driver vulnerable de Capcom, Gigabyte, etc.) para manipular memoria, estos eventos quedan registrados con fecha y hora exacta.

### B. Análisis de `setupapi.dev.log`
- Ubicación: `%SystemRoot%\INF\setupapi.dev.log`
- Registra el historial detallado de instalación de dispositivos, servicios de hardware y controladores.
- **Patrones de Detección**:
  - Razón de instalación marcada como **`Data Overwrite`**: Señal inequívoca de que un instalador forzó la sobrescritura de controladores para encubrimiento o bypass.
  - Búsqueda y presencia de archivos `*.dll.mui` anómalos relacionados con métodos de carga indirecta.

### C. Manipulación de Servicios Críticos del Sistema
- **`PcaSvc` (Program Compatibility Assistant Service)**:
  - Monitorea ejecuciones para aplicar shims.
  - Los scripts de screenshare auditan si `PcaSvc` fue reiniciado deliberadamente (tiempo de actividad del servicio inferior al tiempo de arranque del sistema).
- **`csrss.exe` y `lsass.exe`**:
  - Inspectores como System Informer buscan hilos inyectados, ganchos de API o handles abiertos con permisos `PROCESS_ALL_ACCESS` dirigidos a estos procesos de seguridad.
