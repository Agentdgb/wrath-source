# Bypass Obsoleto / Blatant: Borrado de Shadow Copies y Vaciado de VSS

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Prácticas destructivas típicas de malware y scripts de limpieza masiva que garantizan un baneo permanente en revisiones de screenshare.

---

## 1. Borrado Masivo de Instantáneas con `vssadmin` o `wmic`
- **Técnica errónea**:
  ```cmd
  vssadmin delete shadows /all /quiet
  wmic shadowcopy delete
  powershell.exe "Get-WmiObject Win32_ShadowCopy | ForEach-Object { $_.Delete() }"
  ```
- **Por qué falla ante los analistas (Baneo Inmediato)**:
  - Todo sistema operativo Windows con puntos de restauración activos mantiene un historial de copias de sombra en `vssadmin list shadows`.
  - Esta técnica (idéntica a la que ejecutan los ransomware) destruye todos los puntos de recuperación previos del usuario.
  - Al ejecutar `vssadmin list shadows`, si el comando devuelve *"No items found that satisfy the query"* en un equipo que tiene la protección del sistema habilitada, los analistas de screenshare catalogan la máquina con **`[CRITICAL] Shadow Copies Purged - Active Evidence Destruction` (Auto-Ban)**.
  - Además, se generan eventos de auditoría y de detención en `System.evtx` y `Security.evtx`.

---

## 2. Limitar Forzadamente el Espacio de Almacenamiento de Sombras
- **Técnica errónea**:
  ```cmd
  vssadmin resize shadowstorage /for=C: /on=C: /maxsize=401MB
  ```
- **Por qué falla**:
  - Se utiliza para forzar a Windows a purgar las instantáneas existentes al exceder el límite mínimo.
  - Los analistas auditan el tamaño con `vssadmin list shadowstorage`: un límite modificado a pocos cientos de megabytes en lugar del 5% o 10% habitual del disco es un indicador inmediato de manipulación de políticas de almacenamiento de sombras.

---

## 3. Deshabilitar el Servicio `VSS`
- **Técnica errónea**:
  ```cmd
  sc config vss start= disabled
  net stop vss
  ```
- **Por qué falla**:
  - Viola la **Regla 1** inquebrantable de este proyecto.
  - Impide que Windows cree instantáneas del sistema, provocando fallos en utilidades de copia de seguridad y alertando en cualquier verificación de estado de servicios.
