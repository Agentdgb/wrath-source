# Bypass Obsoleto / Blatant: Borrado de Logs y Vaciado del USN Journal

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Las técnicas más autodestructivas utilizadas en la escena de cheats, que constituyen prueba irrefutable de culpabilidad en cualquier revisión forense.

---

## 1. Vaciado del USN Journal con `fsutil usn deletejournal`
- **Técnica errónea**:
  ```cmd
  fsutil usn deletejournal /d C:
  ```
- **Por qué falla ante `fsutil usn queryjournal C:` (Baneo Inmediato)**:
  - Todo sistema NTFS activo acumula millones de transacciones en su diario USN.
  - Al ejecutar `deletejournal`, el diario se desasigna y reinicia sus contadores.
  - Al ejecutar:
    ```cmd
    fsutil usn queryjournal C:
    ```
    El analista comprueba el campo `LowestValidUsn` y `NextUsn`. Si ve un número diminuto o un diario recién creado en un equipo que lleva días encendido, se declara **`USN Journal Tampered / Deleted`**. Baneo permanente inmediato.

---

## 2. Vaciado de Logs de Eventos con `wevtutil cl`
- **Técnica errónea**:
  ```cmd
  wevtutil cl Security
  wevtutil cl System
  ```
- **Por qué falla ante el Visor de Eventos y Hayabusa (Baneo Inmediato)**:
  - Windows protege la auditoría de seguridad registrando el **Event ID 1102 ("The audit log was cleared")** en `Security.evtx`.
  - El evento detalla el SID exacto de la cuenta de usuario que ejecutó la orden.
  - En `System.evtx` se genera el **Event ID 104**.
  - Este evento es la firma clásica de encubrimiento.

---

## 3. Instalación de Drivers Temporales de Bypass (Event ID 4697)
- **Técnica errónea**: Instalar un driver de kernel mediante un archivo `.bat` o cargador (`sc create cheat_driver binpath= ... type= kernel`).
- **Por qué falla**:
  - Windows registra el **Event ID 4697** en `Security.evtx` con el nombre del servicio, la ruta completa del binario `.sys` y la cuenta que lo instaló.
  - Aunque el archivo `.sys` sea borrado luego, el evento queda grabado de forma permanente en el log de seguridad del kernel.

---

## 4. Manipulación de Claves de Registro en `Services\EventLog`
- **Técnica errónea**: Intentar redirigir el valor `File` o alterar el `Start` de `Services\EventLog` para que Windows no registre eventos.
- **Por qué falla**:
  - Sysmon registra los Event IDs 12 y 13.
  - El subsistema de auditoría de Windows detecta la desincronización y genera advertencias críticas en el arranque.
