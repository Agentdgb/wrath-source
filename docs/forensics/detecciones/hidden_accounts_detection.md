# Detección Forense: Cuentas Ocultas y Persistencia en Registro (SpecialAccounts)

## 1. Identificación y Alcance de la Técnica
- **Técnica**: Hidden Local User Accounts (Cuentas de Usuario Ocultas).
- **Mecanismo**: Creación de cuentas locales en Windows ocultándolas de la pantalla de bienvenida/login mediante claves del registro (`SpecialAccounts\UserList`) o cuentas de puerta trasera (backdoor).
- **Motivo de Auditoría en Screenshares / PC Checks**:
  - Verificar que no existan cuentas secundarias creadas para retener acceso persistente al equipo tras transferencias de cuenta o revisiones.
  - Detectar usuarios paralelos utilizados para compilar o ejecutar software no autorizado sin que aparezca en el perfil visual de inicio.

---

## 2. Métodos de Detección en Screenshares

### A. Enumeración Directa (`net user` y `Get-LocalUser`)
1. **Comando en CMD / PowerShell**:
   ```cmd
   net user
   ```
   o en PowerShell:
   ```powershell
   Get-LocalUser | Select-Object Name, SID, Enabled, LastLogon
   ```
2. **Eficacia**:
   - `net user` y `Get-LocalUser` consultan la base de datos SAM (Security Account Manager) del sistema operativo, **omitiendo cualquier restricción visual de la pantalla de bienvenida**.
   - Si existe una cuenta oculta, aparecerá listada en la consola con su nombre, SID y estado.

### B. Inspección de la Clave `SpecialAccounts\UserList`
- **Ruta del Registro**:
  `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon\SpecialAccounts\UserList`
- **Mecanismo**:
  - Windows permite definir valores de tipo `REG_DWORD` donde el nombre del valor es el nombre de la cuenta de usuario.
  - Si el valor está configurado en `0`, Windows oculta esa cuenta de la pantalla de bloqueo y de inicio de sesión.
- **Bandera de Baneo**:
  - La presencia de la clave `SpecialAccounts\UserList` con una cuenta configurada en `0` es **prueba concluyente de intento de ocultamiento deliberado (Ban Inmediato)**.

### C. Consolas Administrativas (`lusrmgr.msc` y `compmgmt.msc`)
- En `lusrmgr.msc` (Usuarios y grupos locales), el revisor compara la lista de cuentas activas contra los usuarios visibles en la pantalla de bloqueo.
- Revela cuentas creadas y habilitadas que nunca han iniciado sesión de forma visual.

### D. Discrepancias en el Directorio `C:\Users`
- Todo usuario local que haya iniciado sesión tiene su carpeta de perfil en `C:\Users\<Username>`.
- Si el revisor observa carpetas de usuario en `C:\Users\` que no corresponden a ninguna de las cuentas visibles en el login del sistema, se confirma la existencia de un perfil oculto o secundario.

### E. Logs de Eventos de Seguridad (`Security.evtx`)
- **Event ID 4720**: *"A user account was created"*. Registra el nombre de la cuenta creada, el SID de la cuenta que la creó y la marca de tiempo exacta.
- **Event ID 4722**: *"A user account was enabled"*.
- **Event ID 4724**: *"An attempt was made to reset an account's password"*.
- **Event ID 4738**: *"A user account was modified"*.
- Permite demostrar cuándo y quién creó la cuenta, incluso si el usuario intentó camuflarla.
