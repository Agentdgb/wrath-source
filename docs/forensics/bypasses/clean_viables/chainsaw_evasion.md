# Bypass Limpio: Evasión de Chainsaw y Reglas Sigma

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass asegura que al ejecutar `chainsaw hunt` sobre los logs de eventos del sistema, la salida genere un reporte con cero alertas (`0 detections`), preservando la continuidad orgánica de los archivos `.evtx`.

---

## 1. Vector Principal: Cero Generación de Eventos Sospechosos
*Regla operativa de Wrath / VANTA:*

Chainsaw no es un escáner de memoria; es un motor de análisis de reglas sobre registros ya generados en disco. Para superarlo con un puntaje del 100%:

1. **Cero Comandos de Consola / Cero Scripting**:
   - `WrathLoader.exe` no ejecuta scripts de PowerShell ni comandos con sintaxis sospechosa. Cero Eventos 4104 (ScriptBlock Logging) y cero Eventos 4688 con cadenas de alta entropía.
2. **Cero Instalación de Servicios (Zero-Driver)**:
   - Wrath opera en Userland sin drivers de kernel (`.sys`). Cero Eventos 7045 ni 4697 en `System.evtx` / `Security.evtx`.
3. **Servicios Ininterrumpidos (Regla 1)**:
   - La suite nunca detiene servicios críticos (`DPS`, `PcaSvc`, `EventLog`, `SysMain`). Cero Eventos 7036 para servicios del sistema.
4. **Cero Borrado de Logs (Zero-Wipe)**:
   - Jamás se invoca `wevtutil cl` ni se deshabilitan proveedores. Cero Eventos 1102 y cero Eventos 104.
   - **Resultado en Chainsaw**: Al ejecutar el comando:
     ```cmd
     chainsaw hunt C:\Windows\System32\winevt\Logs\ -s sigma\ --mapping mappings\sigma-event-logs-all.yml
     ```
     La herramienta finaliza con **cero coincidencias** en categorías de severidad alta o crítica.

---

## 2. Continuidad Numérica del Registro (`EventRecordID`)
1. Chainsaw incluye analizadores de integridad que detectan la manipulación selectiva de chunks de archivos `.evtx`.
2. Al no interceptar ni manipular los buffers de memoria del servicio `EventLog`, cada evento generado naturalmente en el equipo mantiene una numeración estrictamente correlativa (`EventRecordID` N, N+1, N+2...).
3. No existen huecos temporales (*time gaps*) ni identificadores faltantes que delaten supresión de eventos.

---

## 3. Impersonación Segura y Reversión Atómica (`bam.cpp`)
*Ubicación en el código:* `Bypasses/trace/bam.cpp` (`SystemImpersonationGuard`).

1. Para la mitigación anti-slack de BAM, se requiere acceso temporal a la colmena del sistema mediante duplicación del token de `winlogon.exe`.
2. La clase `SystemImpersonationGuard` asigna el token únicamente al hilo de trabajo (`SetThreadToken`) y lo revierte a `nullptr` inmediatamente al salir del ámbito:
   ```cpp
   ~SystemImpersonationGuard() {
       if (elevated_) SetThreadToken(nullptr, nullptr);
   }
   ```
3. Esta operación en memoria de hilo no genera eventos de inicio de sesión anómalo ni eventos de elevación sospechosa en `Security.evtx`.
