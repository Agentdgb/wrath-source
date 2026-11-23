# Detección Forense: NACIO Kernel Modules Scanner

## 1. Identificación y Alcance de la Herramienta
- **Herramienta**: NACIO Kernel Modules Scanner (`Nacio Kernel Scanner.ps1`).
- **Naturaleza**: Script de auditoría forense para PowerShell desarrollado para la inspección y evaluación de riesgos de controladores de kernel (`.sys`) en Windows.
- **Comando de Invocación Típico en Screenshares**:
  ```powershell
  Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass; Invoke-Expression (Invoke-RestMethod 'https://raw.githubusercontent.com/Nacio78923/Kernel-Scanner/refs/heads/main/Nacio%20Kernel%20Scanner.ps1')
  ```
- **Capacidades Principales**:
  - Detección exhaustiva de archivos `.sys` en disco.
  - Enumeración de módulos de kernel activos en memoria (`SystemModuleInformation`).
  - Verificación estricta de firmas digitales (`Get-AuthenticodeSignature`).
  - Extracción de metadatos de binarios (Compañía, Versión, Descripción).
  - Cálculo de nivel de riesgo (`Risk Level: Safe / Suspicious / Critical`) y exportación a archivo CSV.

---

## 2. Heurísticas y Reglas de Flag en Screenshares
El escáner de NACIO analiza cada módulo de kernel contra las siguientes banderas de alerta:

1. **Drivers Sin Firma o Firma Desconocida (`Status != Valid`)**:
   - En Windows 64-bit (con Driver Signature Enforcement / DSE), cualquier driver cargado en memoria que carezca de firma digital válida emitida por una Autoridad Certificadora reconocida por Microsoft es marcado con riesgo **CRITICAL**.
2. **Drivers Vulnerables Conocidos (BYOVD / LOLDrivers)**:
   - Cruce de hashes y nombres contra la lista de drivers utilizados por mappers de cheats (`gdrv.sys`, `mhyprot2.sys`, `capcom.sys`, `iqvm64.sys`, `kprocesshacker.sys`, `procexp.sys`).
   - Aunque el driver posea una firma de certificado comercial legítima (e.g. Gigabyte, Capcom), si el certificado fue revocado o el driver tiene vulnerabilidades conocidas de lectura/escritura de memoria física, se marca como riesgo **CRITICAL / Banned BYOVD Driver**.
3. **Rutas Anómalas de Almacenamiento**:
   - Los controladores legítimos residen en `%SystemRoot%\System32\drivers`.
   - Controladores alojados en `AppData`, `Temp`, `Desktop`, `Public` o rutas de usuario activan alerta inmediata por **Anomalous Driver Path**.
4. **Metadatos Vacíos o Genéricos**:
   - Falta de campos en la estructura `VS_VERSION_INFO` (`CompanyName` vacío, `Description` sospechosa).
