# Bypass Obsoleto / Blatant: Uso de Drivers BYOVD y Manipulación de Servicios

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Técnicas invasivas y destructivas que delatan intentos de bypass forzado y garantizan la detección inmediata en screenshares.

---

## 1. Carga de Drivers Vulnerables (BYOVD / Mappers de Kernel)
- **Técnica errónea**: Cargar drivers vulnerables de terceros (e.g. `gdrv.sys`, `mhyprot.sys`, `RTCore64.sys`) usando mappers públicos (`kdmapper`, `drvmap`) para manipular memoria del juego o "limpiar" artefactos forenses.
- **Por qué falla (Detección al 100%)**:
  1. **Eventos en Kernel-PnP**: `Event Viewer` registra inmediatamente los Eventos 410, 411 o 420 en `Microsoft-Windows-Kernel-PnP/Device Management` con el nombre del servicio del driver temporal.
  2. **`setupapi.dev.log`**: Deja constancia de la creación del servicio del controlador.
  3. **Listas de Bloqueo de Drivers**: Herramientas como Echo y Paladin auditan los drivers cargados contra la lista de certificados revocados y drivers vulnerables conocidos (Vulnerable Driver Blocklist). Encontrar un driver de este tipo en la pestaña de drivers de System Informer equivale a **Ban Inmediato por Kernel Bypass Attempt**.

---

## 2. Detención o Reinicio Forzado de `PcaSvc`
- **Técnica errónea**:
  ```cmd
  net stop pcasvc
  taskkill /f /fi "services eq pcasvc"
  ```
- **Por qué falla (Flag Directo de Servicios)**:
  - **Violación de la Regla 1**: Servicios críticos jamás deben detenerse.
  - Los scripts de screenshare verifican el tiempo de inicio del servicio `PcaSvc` contra el tiempo de encendido del equipo. Si `PcaSvc` muestra un uptime inferior o fue iniciado hace 20 minutos, el script emite la alerta **`PcaSvc Restarted / Manipulated`**.
  - Se genera el Evento 7036 en `System.evtx`.

---

## 3. Sobrescritura Forzada de `setupapi.dev.log`
- **Técnica errónea**: Editar manualmente o utilizar scripts para reescribir líneas en `%SystemRoot%\INF\setupapi.dev.log`.
- **Por qué falla**:
  - El instalador del sistema de Windows verifica la coherencia del log. Si se detectan anomalías de formateo o líneas que indican `Data Overwrite`, los inspectores forenses identifican que hubo un intento de encubrimiento de dispositivos.
