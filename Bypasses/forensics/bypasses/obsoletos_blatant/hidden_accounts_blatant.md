# Bypass Obsoleto / Blatant: Creación de Cuentas Ocultas con SpecialAccounts

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Una técnica arcaica de foros públicos de persistencia que delata culpabilidad inmediata en cualquier revisión de screenshare o auditoría de cuentas.

---

## 1. Crear Cuentas y Ocultarlas con `SpecialAccounts\UserList`
- **Técnica errónea**:
  ```cmd
  net user hidden_admin P@ssw0rd123 /add
  net localgroup administrators hidden_admin /add
  reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon\SpecialAccounts\UserList" /v hidden_admin /t REG_DWORD /d 0 /f
  ```
- **Por qué falla ante los analistas (Baneo Inmediato)**:
  1. **Inutilidad ante la Consola**:
     - Ocultar la cuenta en `SpecialAccounts\UserList` **solo afecta a la interfaz gráfica de la pantalla de bienvenida**.
     - Cuando el revisor abre CMD y tipea:
       ```cmd
       net user
       ```
       o ejecuta `Get-LocalUser` en PowerShell, la cuenta `hidden_admin` aparece en pantalla en la primera línea.
  2. **Auditoría de Registro Automática**:
     - Los escáneres de screenshare leen directamente la clave `SpecialAccounts\UserList`.
     - Encontrar esta clave configurada con un valor `0` es causal directa de **Ban Permanente por Backdoor / Hidden Account Attempt**.
  3. **Eventos en `Security.evtx`**:
     - El kernel registra el **Event ID 4720** ("A user account was created") con el nombre de la cuenta y el timestamp de cuándo fue creada.
  4. **Perfil en `C:\Users`**:
     - Si la cuenta inició sesión alguna vez, `C:\Users\hidden_admin` delata la existencia física de la cuenta en disco.
