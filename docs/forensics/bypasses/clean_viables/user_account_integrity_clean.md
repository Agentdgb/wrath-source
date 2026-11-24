# Bypass Limpio: Integridad de Cuentas de Usuario y Cero Persistencia

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass asegura que el sistema mantenga una estructura de cuentas de usuario 100% legítima y armónica, rechazando la creación de usuarios ocultos o puertas traseras que son detectadas instantáneamente por `net user` y el registro.

---

## 1. Vector Principal: Cero Creación de Cuentas (Zero-Account Philosophy)
*Regla de arquitectura de Wrath / VANTA:*

1. **Operación en el Contexto de la Sesión Activa**:
   - Wrath / VANTA opera estrictamente dentro de la sesión de usuario activa del jugador.
   - **NUNCA crea cuentas de usuario locales, cuentas de servicio ni perfiles secundarios**.
2. **Impacto en los Métodos de Detección**:
   - **En `net user` y `Get-LocalUser`**:
     - Solo aparecen las cuentas legítimas predeterminadas del sistema (`Administrator`, `DefaultAccount`, `Guest`, `WDAGUtilityAccount`) y la cuenta personal del usuario.
     - Cero cuentas no reconocidas en el SAM.
   - **En el Registro (`Winlogon\SpecialAccounts`)**:
     - La subclave `SpecialAccounts\UserList` **NO existe** o se encuentra en su estado predeterminado de Windows, sin valores `0` que oculten perfiles.
   - **En `C:\Users`**:
     - Solo existen las carpetas de perfil auténticas del jugador y las carpetas por defecto del sistema (`Default`, `Public`).
     - Cero discrepancias entre carpetas de disco y usuarios del sistema.
   - **En `Security.evtx`**:
     - Cero registros de **Event ID 4720** (Account Created) ni **Event ID 4722** (Account Enabled) generados durante la sesión de juego.

---

## 2. Impersonación Atómica de Token a Nivel de Hilo (`bam.cpp`)
*Ubicación en el código:* `Bypasses/trace/bam.cpp` (`SystemImpersonationGuard`).

1. Cuando la suite requiere privilegios elevados temporales para realizar mitigaciones anti-slack en la colmena del sistema (`SYSTEM` hive):
   - **No crea una cuenta administrativa oculta**.
   - Duplica temporalmente el token de seguridad del proceso local del sistema `winlogon.exe` mediante `DuplicateTokenEx`.
   - Asigna el token **exclusivamente al hilo de trabajo en ejecución** con `SetThreadToken`.
   - Al finalizar la operación de escritura, el destructor `~SystemImpersonationGuard()` revierte inmediatamente el token a `nullptr` (`SetThreadToken(nullptr, nullptr)`).
2. Esta técnica opera íntegramente en memoria volátil de hilo, sin modificar la base de datos de usuarios de Windows ni generar eventos de autenticación anómalos.
