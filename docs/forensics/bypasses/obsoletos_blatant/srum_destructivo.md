# Bypass Obsoleto / Blatant: Detención de DPS y Vaciado de SRUDB

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Métodos destructivos que causan banderas rojas automáticas en revisiones forenses y violan directamente las directrices de seguridad de Wrath / VANTA.

---

## 1. Detener o Deshabilitar el Servicio `DPS` (Diagnostic Policy Service)
- **Técnica errónea**: Ejecutar comandos como:
  ```cmd
  net stop dps
  sc stop dps
  sc config dps start= disabled
  ```
- **Por qué falla (Flag Inmediato)**:
  - **Violación Crítica de Reglas**: Viola la Regla 1 del proyecto.
  - **Uptime Audit**: Herramientas como Echo, Paladin y Ocean auditan el estado y el tiempo de actividad (`uptime`) de `DPS` frente al tiempo transcurrido desde el arranque del equipo (`GetTickCount64`). Si `DPS` está detenido o su tiempo activo no coincide con el arranque del sistema, se genera un **flag crítico de manipulación de servicios**.
  - **Evento 7036**: El administrador de control de servicios registra el Evento 7036 en `System.evtx` ("The Diagnostic Policy Service entered the stopped state"), dejando una prueba irrefutable de que fue apagado manualmente.

---

## 2. Borrar o Truncar `SRUDB.dat`
- **Técnica errónea**: Forzar el borrado o reemplazo de:
  ```cmd
  del /f /q C:\Windows\System32\sru\SRUDB.dat
  ```
- **Por qué falla**:
  - Al reiniciar el servicio o reiniciar el sistema, Windows genera una nueva base `SRUDB.dat` vacía.
  - Un sistema Windows normal con horas o días de uso acumula cientos de megabytes de telemetría de navegadores, explorador y procesos del sistema. Una base de datos `SRUDB.dat` con tamaño mínimo (pocos kilobytes) o con marcas de tiempo recientes en un equipo que lleva horas encendido es catalogada por SrumECmd y los analistas como **"Wiped SRUM Evidence" (Baneo instantáneo)**.
  - Deja registros de borrado del archivo en `$LogFile` y `$MFT`.

---

## 3. Alteración Tosca de Registro de Extensiones SRUM
- **Técnica errónea**: Modificar o eliminar subclaves en:
  `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\SRUM\Extensions`
- **Por qué falla**:
  - Rompe la estructura de GUIDs esperada por Windows.
  - El servicio `DPS` arroja excepciones internas y deja de recolectar telemetría general, lo que activa advertencias de integridad en cualquier script de auditoría de Windows.
