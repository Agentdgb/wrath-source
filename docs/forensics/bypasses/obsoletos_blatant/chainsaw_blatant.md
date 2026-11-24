# Bypass Obsoleto / Blatant: Borrado de Logs y Modificación de .evtx

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Técnicas destructivas o burdas que activan las reglas Sigma más severas (`CRITICAL`) dentro de **Chainsaw**.

---

## 1. Vaciado de Logs con `wevtutil cl`
- **Técnica errónea**:
  ```cmd
  wevtutil cl Security
  wevtutil cl System
  wevtutil cl "Microsoft-Windows-PowerShell/Operational"
  ```
- **Por qué falla ante Chainsaw (Baneo Inmediato)**:
  - Chainsaw incluye la regla Sigma oficial:
    `sigma/rules/windows/builtin/security/win_security_audit_log_cleared.yml`
  - Esta regla busca específicamente el `Event ID 1102`.
  - Al procesar el archivo `Security.evtx`, Chainsaw resalta el resultado en color rojo parpadeante con el nivel más alto de amenaza:
    **`CRITICAL: Security Audit Log Was Cleared`**.
  - El reporte imprime el usuario, dominio y la hora exacta en que se ejecutó la orden de vaciado.

---

## 2. Detención de Servicios del Sistema
- **Técnica errónea**: Detener `PcaSvc` o `DPS` con `net stop` para evitar que Windows registre ejecuciones.
- **Por qué falla ante Chainsaw**:
  - Chainsaw monitorea el `Event ID 7036` en `System.evtx` mediante la regla Sigma:
    `win_system_service_stopped_suspicious.yml`.
  - Si un servicio de auditoría o diagnóstico pasa a estado detenido sin una orden de reinicio del sistema, Chainsaw marca la acción como evasión activa de defensa (*Defense Evasion / Service Tampering*).

---

## 3. Poda Manual o Parcheo de Archivos `.evtx`
- **Técnica errónea**: Intentar abrir los archivos `.evtx` con editores hexadecimales o scripts para eliminar registros individuales de inyección.
- **Por qué falla**:
  - El formato `.evtx` agrupa los eventos en trozos binarios llamados *Chunks* de 64 KB, protegidos por sumas de verificación CRC32 tanto en la cabecera del archivo como en la cabecera de cada chunk.
  - Además, cada evento tiene un número correlativo secuencial (`EventRecordID`).
  - Al editar o borrar un evento manualmente:
    - Se rompe el checksum del chunk, haciendo que Chainsaw reporte **`Corrupt EVTX Chunk Detected`**.
    - Se genera un salto en la secuencia numérica (e.g. del evento 512 pasa al 515), disparando la alerta de Chainsaw por **`Event Record ID Sequence Gap - Tampering Indicator`**.
