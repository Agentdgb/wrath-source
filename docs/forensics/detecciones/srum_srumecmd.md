# Detección Forense: SrumECmd y Análisis de SRUM (SRUDB.dat)

## 1. Identificación y Alcance de la Herramienta
- **Herramienta**: SrumECmd (Eric Zimmerman Tools)
- **Objetivo**: Parser forense de la base de datos de telemetría y rendimiento **SRUM** (System Resource Usage Monitor) de Windows.
- **Ubicación del artefacto**: `%SystemRoot%\System32\sru\SRUDB.dat` (base de datos en formato ESE / Extensible Storage Engine / Jet Blue).
- **Consumo**: Exportación a CSV categorizados y visualización en Timeline Explorer.

---

## 2. Tablas y Métricas Forenses Extraídas
SrumECmd decodifica los GUIDs de tablas en `SRUDB.dat` y extrae:

1. **`{D10CA2FF-5790-449E-902C-1F12E1AE4F62}` — App Resource Usage**:
   - `Application`: Nombre o ruta completa del ejecutable (resuelto desde la tabla `SruDbIdMapTable`).
   - `UserId`: SID del usuario que ejecutó la aplicación.
   - `ForegroundCycleTime` y `BackgroundCycleTime`: Ciclos exactos de CPU consumidos en primer y segundo plano.
   - `FaceTime`: Duración en segundos que la ventana estuvo en primer plano.
   - `BytesRead` / `BytesWritten`: E/S de disco acumulada.
   - `ContextSwitches`: Frecuencia de cambio de contexto del procesador.
2. **`{973F5D5C-1D90-4944-BE8E-24B94231A174}` — Network Data Usage**:
   - Conexiones de red, bytes enviados y recibidos por adaptador (Wi-Fi, Ethernet, Loopback) asociados al binario.
3. **`{DD6636C4-029D-46F5-845F-56044B8880C2}` — Network Connectivity**:
   - Duración de la conexión a internet de la aplicación.
4. **`{FEE05495-AE85-45F3-8034-BE94B0A9BB09}` — Long Term Energy Usage**:
   - Consumo estimado de batería/energía.

---

## 3. Heurísticas y Reglas de Flag en Screenshares (Echo, Paladin, Ocean)
- **Flag 1 — Ejecutable Desconocido en Carpetas de Usuario**: Procesos ejecutados desde `AppData`, `Temp`, directorios de descargas o carpetas no estándar con registros de consumo de CPU/red en las últimas 24–48 horas.
- **Flag 2 — Horarios Anómalos**: Aplicaciones de fondo ejecutadas durante la madrugada o en intervalos donde el usuario supuestamente no estaba frente al equipo.
- **Flag 3 — Spikes de Red/CPU Desconectados de la Interfaz**: Ejecutables sin ventana gráfica (`FaceTime = 0`) que consumen recursos de CPU sostenidos o transfieren datos por red (típico de inyectores o loaders con DRM ruidoso).
- **Flag 4 — Base SRUDB.dat Vacía o Truncada**: Si `SRUDB.dat` no contiene historial previo o fue reseteada, se asume manipulación deliberada.
