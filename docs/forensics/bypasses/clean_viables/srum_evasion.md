# Bypass Limpio: Evasión de SRUM (SRUDB.dat / SrumECmd)

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass permite evadir la detección en `SRUDB.dat` sin detener el servicio `DPS` ni alterar la integridad de la base de datos Jet Blue del sistema.

---

## 1. Vector Principal: Absorción de Carga en el Proceso Anfitrión (`javaw.exe`)
*Mecanismo intrínseco de Wrath / VANTA Client:*

1. **Atribución en SRUM**:
   - Windows SRUM solo registra la actividad a nivel de proceso (`EPROCESS`).
   - Una vez que `WrathPayload.dll` es inyectado (mediante manual mapping o module stomping) dentro del proceso de Minecraft (`javaw.exe`), **todo el consumo de CPU, GPU, memoria y red es atribuido contablemente a `javaw.exe`**.
2. **Invisibilidad del Payload**:
   - `WrathPayload.dll` no es un proceso independiente; no posee `AppId` en `SruDbIdMapTable`.
   - Minecraft es un juego intensivo en recursos: CPU multicore, renderizado OpenGL/DirectX y tráfico constante de paquetes de red. Los micro-ciclos que consume Wrath (ESP, Aimbot, Packet hooks) son indistinguibles del consumo natural del juego.

---

## 2. Vector Secundario: Mimetización del Loader y Ciclo Efímero
*Mecanismo del Loader Wrath:*

1. **Ruta y Nombre Señuelo**:
   - El ejecutable del loader (`WrathLoader.exe`) se despliega bajo una identidad y firma legítima de utilidad administrativa (e.g. `DisplayTunerService.exe` o `BackupManager.exe`).
2. **Ciclo de Inyección Ultrarrápido (< 500 ms)**:
   - El loader se ejecuta, resuelve los módulos, inyecta el payload en el proceso objetivo y finaliza inmediatamente (`ExitProcess`).
   - El servicio `DPS` recopila métricas en memoria y solo realiza un "flush" a disco cada ~60 minutos. La huella de un binario que ejecuta durante 200 ms con menos de 10 MB de RAM y consumo insignificante de ciclos de CPU queda sepultada en el ruido de fondo o ni siquiera califica para las agregaciones estadísticas de alto consumo.
3. **Cero Conexiones de Red Ruidosas**:
   - El loader no debe mantener conexiones HTTP/TCP en segundo plano durante el juego. La autenticación o verificación de licencia debe resolverse en un único handshake cifrado efímero.

---

## 3. Manipulación Atómica ESE / JetDB en Reposo (Edición Quirúrgica sin Detener Servicios)
Si por algún motivo el loader requiriera ser purgado de un historial previo de `SRUDB.dat`:

1. **Nunca detener `DPS`**.
2. **Sombra VSS / Extracción en Memoria**:
   - Se crea un snapshot de volumen (VSS) o se abre un handle con `FILE_SHARE_READ | FILE_SHARE_WRITE`.
   - Se parsea el archivo de base de datos ESE mediante la API nativa de Windows `esent.dll` (`JetInit`, `JetOpenDatabase`, `JetDelete`).
   - Se localiza el registro en `SruDbIdMapTable` que mapea el ID del binario específico y se eliminan exclusivamente sus filas asociadas en la tabla `{D10CA2FF-5790-449E-902C-1F12E1AE4F62}`.
   - Se recalculan los checksums de página de la base ESE para evitar que el motor Jet detecte corrupción de base de datos (`JET_errDatabaseCorrupted`).
   - *Nota*: La absorción en `javaw.exe` y la mimetización son la primera línea recomendada; la edición quirúrgica solo se utiliza como contingencia extrema.
