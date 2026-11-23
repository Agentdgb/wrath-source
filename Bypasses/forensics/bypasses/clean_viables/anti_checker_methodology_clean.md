# Arquitectura Limpia frente a Metodologías de Screensharing (Wrath / VANTA)

## 1. Principio Fundamental: Preservación de la Naturalidad del Sistema
El principio operativo central de **Wrath / VANTA** es la **naturalidad del entorno**:
> *"Un sistema con registros vacíos o servicios apagados es un sistema culpable. La evasión limpia no destruye la evidencia; genera un estado indistinguible de la actividad ordinaria de un usuario legítimo."*

Al comprender los sesgos y metodologías de los checkers (como la falacia de "sin huellas = limpio" o la correlación cruzada de artefactos), la arquitectura de Wrath se diseña para no desencadenar ninguna anomalía estructural.

---

## 2. Mecanismos de Evasión Alineados con la Metodología

### A. Doctrina de "Presencia Natural" (Anti-Empty Trace Flag)
* **Vulnerabilidad del Checker**: El checker audita `Prefetch`, `BAM`, `EventLog` y `USN Journal` buscando borrados masivos o registros anormalmente limpios.
* **Implementación Wrath**:
  * **Servicios 100% Intactos**: `DPS`, `PcaSvc`, `EventLog`, `SysMain`, `DiagTrack` y `Schedule` permanecen en estado `Running` durante todo el ciclo de vida del proceso.
  * **Zero Wipe Masivo**: Nunca se ejecutan comandos de limpieza como `wevtutil cl`, `fsutil usn deletejournal` ni vaciados de `C:\Windows\Prefetch`.
  * **Teardown Quirúrgico**: Mediante `wrath::trace::teardown`, únicamente se limpian o corrigen las entradas asociadas a la sesión efímera del loader en BAM y Prefetch, preservando intacto el 99.9% restante del historial del sistema.

### B. Neutralización de la Correlación Dual
* **Vulnerabilidad del Checker**: El analista no confía solo en ShimCache; busca correlacionar la presencia del binario en BAM, Prefetch y MFT.
* **Implementación Wrath**:
  * El loader opera desde un contenedor VHD montado con las banderas `ATTACH_VIRTUAL_DISK_FLAG_NON_PNP (0x40)` y `ATTACH_VIRTUAL_DISK_FLAG_NO_LOCAL_HOST (0x08)`.
  * Al no tener letra de unidad ni asociarse al volumen host `C:\`:
    * No genera entradas de archivo en el `$MFT` del disco principal.
    * No genera registros en el `$LogFile` ni en `$Extend\$UsnJrnl` de `C:\`.
  * Para los componentes que tocan el espacio del host, se utilizan binarios señuelo (`DisplayTunerService.exe`) con cabeceras PE genuinas, hashes consistentes y salt criptográfico por compilación (`build_salt.h`), lo que hace que cualquier residuo correlacione como software utilitario legítimo.

### C. Respeto al Orden de Volatilidad (Zero Volatile Footprint)
* **Vulnerabilidad del Checker**: La primera fase de una auditoría en caliente es inspeccionar la RAM y las conexiones de red.
* **Implementación Wrath**:
  * **Memoria de Inyección**: El payload (`WrathPayload.dll`) se mapea en `javaw.exe` mediante `SEC_IMAGE` respaldado por Transacted NTFS (`NtCreateSection` + `RollbackTransaction`).
    * No deja páginas en `MEM_PRIVATE` con permisos `PAGE_EXECUTE_READWRITE` (RWX).
    * El tipo de memoria figura como `MEM_IMAGE`, exactamente igual que una DLL cargada formalmente por el sistema operativo.
  * **Scrubbing de Cadenas**: Todas las cadenas sensibles se compilan con cifrado polimórfico XOR (`enc.h` / `StackString`) y se sobrescriben en memoria mediante `StringScrub.h` (`RtlSecureZeroMemory`) tras su inicialización. Un volcado de RAM con `WinPmem` o `bstrings` no produce cadenas de cheats ni URLs de webhooks.
  * **Sockets de Red**: El cliente no mantiene conexiones TCP/UDP abiertas contra servidores de licencias durante el juego; la autenticación es inicial, atómica y localmente descartada.

### D. Consistencia Temporal Absoluta
* **Vulnerabilidad del Checker**: Buscar desajustes temporales entre `$STANDARD_INFORMATION` y `$FILE_NAME`, o desfases entre eventos de Prefetch y el reloj del sistema.
* **Implementación Wrath**:
  * La herramienta de sincronización temporal de Wrath (`stamps.cpp`) actualiza simultáneamente ambos atributos de marca de tiempo (`$SI` y `$FN`) mediante llamadas nativas de bajo nivel (`NtSetInformationFile` con `FileBasicInformation`).
  * No se altera el reloj del sistema (`SetSystemTime`), evitando discontinuidades en los identificadores secuenciales de eventos de Windows (`EventRecordID`).

### E. Entorno Físico Auténtico (Bare-Metal Guarantee)
* **Vulnerabilidad del Checker**: Chequeo de hypervisores, VMs, adaptadores de red virtuales y dispositivos PCI secundarios (DMA/KMBOX).
* **Implementación Wrath**:
  * Opera al 100% en espacio de usuario (Ring 3) en una instalación física nativa de Windows (Bare-Metal).
  * No requiere controladores vulnerables de terceros (BYOVD).
  * `HypervisorPresent` permanece en `False` (o con el hypervisor nativo de Hyper-V/VBS de Microsoft en estado estándar).
  * No existen dispositivos seriales extraños en el bus USB ni tarjetas de captura DMA.

---

## 3. Matriz de Comportamiento ante Screenshare en Vivo

| Fase del Checker | Reacción Ingenua (Blatant) | Arquitectura Wrath (Limpia) |
|---|---|---|
| **Petición de Entrada** | Pedir reiniciar el PC o cerrar procesos con prisa. | Aceptar inmediatamente el screenshare; cero procesos de pánico. |
| **Inspección de Servicios** | 1 o más servicios clave detenidos (`PcaSvc`, `SysMain`). | Todos los 11 servicios obligatorios verificados en `Running` (`services.cpp`). |
| **Inspección de Prefetch** | Carpeta vacía, archivos `.pf` de 0 bytes o fecha de hoy para todo. | Historial íntegro de meses de uso normal; solo el señuelo figura como herramienta de utilidad. |
| **Escaneo de Memoria RAM** | Bloques de código RWX sin respaldar; cadenas `"reach"` en texto plano. | Memoria de tipo `MEM_IMAGE`; cadenas cifradas en compilación y saneadas tras carga. |
| **Volcado de MFT / USN** | Transacciones de borrado masivo registradas en `$LogFile`. | Cero escrituras en el volumen del host; contenedor VHD desmontado limpiamente sin rastro de desasignación. |
