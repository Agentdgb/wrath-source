# Metodología y Errores Críticos de Checkers / Screensharers

## 1. Visión General del Vector
Durante una revisión manual o asistida por herramientas (Echo, Paladin, Ocean, Avenge, Red Lotus), la fiabilidad del veredicto depende estrictamente del rigor metodológico del analista. Un checker inexperto comete fallos lógicos y operativos sistemáticos que invalidan la prueba o permiten falsos positivos y falsos negativos.

Los evaluadores experimentados y las directrices forenses de DFIR establecen un marco estricto para evitar sesgos cognitivos, destrucción de evidencia volátil y trampas de correlación.

---

## 2. Los Diez Errores Críticos en la Investigación Forense

### 1. Confiar en un Único Artefacto (Single-Point Fallacy)
* **Error**: Identificar una entrada en `AppCompatCache` (ShimCache) o `Amcache.hve` y concluir inmediatamente ejecución maliciosa activa.
* **Realidad Técnica**:
  * ShimCache registra la mera presencia o interacción del archivo en el sistema de archivos (por ejemplo, indexación, apertura en Explorer o análisis estático), no necesariamente su ejecución continuada o inyección.
  * AmCache puede retener hashes de binarios compilados que nunca llegaron a vincularse a un espacio de direcciones de destino.
* **Regla Forense**: Correlación dual obligatoria. Todo indicio de ShimCache o Amcache debe ser confirmado por al menos un artefacto de ejecución independiente y contemporáneo: Prefetch (`.pf`), Background Activity Moderator (`BAM`/`DAM`), System Resource Usage Monitor (`SRUM`), o `UserAssist`.

### 2. Destrucción de Datos Volátiles (Inversión del Orden de Volatilidad)
* **Error**: Indicar al usuario: *"Cierra el juego/navegador y reinicia el PC para pasar un scanner en frío"*.
* **Realidad Técnica**:
  * La RFC 3227 define la escala de volatilidad: Registros/Caché de CPU → Memoria RAM (Working Set/VAD) → Estado de Red (Sockets/TCP) → Estado del Kernel/Servicios → Almacenamiento en Disco.
  * Reiniciar el sistema destruye instantáneamente:
    * Módulos cargados en memoria privada (`MEM_PRIVATE` con `PAGE_EXECUTE_READWRITE`).
    * Conexiones de sockets no cerradas (`GetExtendedTcpTable`).
    * Manijas de procesos (`Process Handles`) y secciones TxF.
    * Entradas de Prefetch pendientes de vaciado a disco (`pf_dump_interval`).
* **Regla Forense**: Adquisición prioritaria de memoria en caliente (`DumpIt`, `WinPmem`, `Process Hacker`) y sockets antes de cualquier modificación en el estado del host.

### 3. Ceder el Control del Sistema al Usuario
* **Error**: Observar pasivamente mientras el usuario comparte pantalla y navega por el explorador de archivos.
* **Realidad Técnica**:
  * Permite al usuario disparar scripts de pánico vía macros de teclado/ratón (atajos globales, `RegisterHotKey`).
  * Facilita cambios de ventana rápidos (*alt-tabbing*) a escritorios virtuales secundarios (`IVirtualDesktopManager`) donde se desmontan contenedores o se limpian carpetas.
  * Permite la ejecución de desasociaciones rápidas de red.
* **Regla Forense**: Control remoto absoluto vía software verificado (AnyDesk, TeamViewer) con todos los monitores transmitidos simultáneamente, prohibiendo entradas físicas del usuario durante la sesión y registrando la auditoría completa en vídeo.

### 4. La Falacia de "Sin Huellas = Limpio" (Negative Evidence Paradox)
* **Error**: Abrir `C:\Windows\Prefetch`, encontrarlo vacío o con menos de 10 entradas, y declarar al usuario "inocente".
* **Realidad Técnica**:
  * En un sistema Windows operativo, la ausencia de artefactos es, en sí misma, un artefacto de alteración intencionada (*anti-forensics flag*).
  * Prefetch se purga de forma natural únicamente por políticas de espacio o rotación (máximo 1024/128 archivos). Un vaciado completo implica `del /q *` o parada de `SysMain`.
  * Registros de eventos sin entradas antiguas con `EventRecordID` bajo indican `wevtutil cl` o truncado de chunks `.evtx`.
* **Regla Forense**: Evaluar la continuidad histórica. La falta de rastros esperados es causa inmediata de sanción por manipulación evidente.

### 5. Falsos Positivos por Coincidencia Parcial de Cadenas
* **Error**: Ejecutar un escaneo de memoria buscando la palabra `"reach"`, `"hitbox"`, `"autoclicker"` o un webhook de Discord, encontrándola en la caché de Google Chrome o en el historial de Windows Defender, y concluir infracción.
* **Realidad Técnica**:
  * Navegar por páginas web, ver vídeos de YouTube sobre revisiones de cheats, o recibir advertencias de Windows Defender deposita cadenas de texto idénticas en la memoria del navegador o en los registros de cuarentena de `MpSigStub.exe` / `windefend`.
* **Regla Forense**: Contextualización del proceso. Las cadenas deben hallarse en el espacio de direcciones del proceso objetivo (el cliente del juego) o en un proceso inyector con manija abierta hacia este (`PROCESS_VM_READ | PROCESS_VM_WRITE`).

### 6. Visión de Túnel (Tunnel Vision en el Directorio del Juego)
* **Error**: Limitar el escaneo exclusivamente a `.minecraft/versions/`, `FiveM/FiveM.app/` o carpetas de configuración del cliente.
* **Realidad Técnica**:
  * Ningún inyector o módulo moderno reside en las carpetas base del juego.
  * Los ejecutables se ubican en rutas no indexadas, volúmenes secundarios, carpetas de desarrollo (`AppData\Local\Temp`), o montajes en memoria.
* **Regla Forense**: Auditoría global del sistema basada en líneas de tiempo (Timeline MFT, USN Journal) y no en inspección visual de carpetas seleccionadas.

### 7. Ignorar Hardware, DMA y Dispositivos Externos
* **Error**: Limitar la revisión a software en Ring 3 ignorando la topología de bus y periféricos.
* **Realidad Técnica**:
  * Dispositivos de hardware externos como KMBOX (Net/B+ Pro) actúan como controladores de ratón por hardware sin software local en ejecución activa.
  * Dispositivos PCIe DMA (Direct Memory Access) leen y escriben memoria física desde otra máquina.
* **Regla Forense**: Inspección de `setupapi.dev.log`, `DevManView` y enumeración de dispositivos USB y buses PCI en busca de VID/PID extraños o tarjetas FTDI/CH340/CH341 no reconocidas.

### 8. Omitir Particiones, Contenedores y Rutas Fantasma
* **Error**: Revisar únicamente el volumen `C:\` montado en el árbol estándar.
* **Realidad Técnica**:
  * El uso de VHDs (`.vhdx`) montados sin letra de unidad (`ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER`), sistemas multi-boot, o carpetas `Windows.old` preservadas permite operar entornos paralelos.
* **Regla Forense**: Enumerar volúmenes lógicos con `mountvol`, comprobar metadatos de BitLocker (`manage-bde`), y revisar descriptores de dispositivos en `\\.\PhysicalDriveX`.

### 9. Ignorar Líneas de Tiempo y Sincronización Temporal
* **Error**: Encontrar un archivo sospechoso con fecha de hace 6 meses y asociarlo a la sesión de juego de hoy, o ignorar un desfase temporal entre el reloj del sistema y los logs.
* **Realidad Técnica**:
  * Desajustes intencionados del reloj (`SetSystemTime`) para engañar a los clasificadores basados en *"últimas 24 horas"*.
  * Timestomping descuidado donde `$STANDARD_INFORMATION` difiere de `$FILE_NAME`.
* **Regla Forense**: Correlación estricta entre el evento dentro del servidor de juego y la ventana de ejecución exacta registrada en logs del sistema correlacionados en UTC.

### 10. Aceptar Explicaciones Verbales sin Respaldo Forense
* **Error**: Permitir que el usuario justifique un servicio detenido o una clave de registro ausente diciendo: *"Es que optimicé mi Windows con un pack de FPS de YouTube"*.
* **Realidad Técnica**:
  * Los "packs de optimización" son el pretexto habitual utilizado para deshabilitar `EventLog`, `PcaSvc` o `DPS`.
* **Regla Forense**: El estándar de competencia prohíbe modificaciones que destruyan la integridad de los registros del sistema operativo. Si el sistema no es auditable por decisión del usuario, el veredicto es no apto / sanción técnica.
