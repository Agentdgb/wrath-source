# Bypass Obsoleto / Blatant: Uso de Drivers BYOVD y Desvinculación de Kernel

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Técnicas invasivas basadas en controladores de kernel públicos que activan de inmediato alertas de nivel crítico en escáneres como NACIO y System Informer.

---

## 1. Carga de Drivers Vulnerables (BYOVD - Bring Your Own Vulnerable Driver)
- **Técnica errónea**: Utilizar utilidades públicas de carga de drivers (`kdmapper`, `drvmap`) para cargar controladores con firmas antiguas pero vulnerables (e.g. `gdrv.sys`, `mhyprot2.sys`, `capcom.sys`, `RTCore64.sys`) con el objetivo de leer memoria o desactivar protecciones.
- **Por qué falla ante NACIO Kernel Scanner (Baneo Inmediato)**:
  - NACIO verifica los hashes y nombres de archivo contra bases de datos de vulnerabilidades conocidas (LOLDrivers y listas de bloqueo de Microsoft).
  - En cuanto el script detecta la presencia en memoria de uno de estos drivers, lo clasifica automáticamente como **`Risk Level: CRITICAL (Vulnerable Driver Detected)`**.
  - El revisor observa la fila en rojo brillante en el CSV y emite el baneo de forma instantánea.

---

## 2. Desvincular el Driver de `PsLoadedModuleList` (DKOM)
- **Técnica errónea**: Usar un exploit de kernel para recorrer la lista doblemente enlazada `PsLoadedModuleList` y desvincular el nodo del controlador para que "no aparezca" en la lista de módulos.
- **Por qué falla**:
  - Desvincular la lista enlazada no elimina el objeto del controlador (`DRIVER_OBJECT`) del Object Manager de Windows (`\Driver\...`).
  - Escáneres forenses avanzados (como Volatility con el plugin `driverscan` o herramientas de inspección de objetos del kernel) recorren el directorio de objetos y la memoria de los pools.
  - Al encontrar un `DRIVER_OBJECT` activo en memoria que no está registrado en `PsLoadedModuleList`, se detecta la firma definitiva de un **Rootkit Oculto (Hidden Driver Anomaly)**.

---

## 3. Bloquear o Interceptar la URL del Script de NACIO
- **Técnica errónea**: Editar el archivo `hosts` o configurar el firewall local para bloquear `raw.githubusercontent.com`, buscando que el comando de PowerShell del revisor arroje error de conexión y no pueda descargarse.
- **Por qué falla**:
  - Los revisores ejecutan este comando rutinariamente; si el comando falla con `WebException` o `Unable to connect to remote server`, el revisor inmediatamente sospecha de bloqueo deliberado.
  - El revisor prueba abrir el enlace en el navegador o ejecuta scripts locales predescargados en su USB/AnyDesk. Si comprueba que la máquina bloquea deliberadamente los repositorios de herramientas de revisión, el baneo es automático por **Obstrucción y Sabotaje de Screenshare**.
