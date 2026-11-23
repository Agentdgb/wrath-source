# Detección Forense: Velociraptor (VQL, YARA en Memoria y Caza Volátil)

## 1. Identificación y Alcance de la Plataforma
- **Plataforma**: Velociraptor (v0.73+ — Despliegue MSI / CLI).
- **Naturaleza**: Sistema avanzado de Digital Forensics and Incident Response (DFIR) y Threat Hunting en tiempo real.
- **Motor de Consulta**: VQL (Velociraptor Query Language). Permite ejecutar consultas estructuradas en vivo sobre endpoints para recolectar y correlacionar artefactos volátiles y persistentes.
- **Capacidades Críticas en Screenshares / Revisiones**:
  - Escaneo de memoria RAM en vivo mediante reglas **YARA** dirigidas a procesos (`accessor = "process"`).
  - Detección de inyecciones de memoria (Shellcode, Reflective DLL, Process Hollowing).
  - Inspección de artefactos volátiles: sockets de red activos, handles abiertos y páginas de memoria virtual.

---

## 2. Artefactos VQL y Consultas de Caza en Vivo

### A. Escaneo YARA Directo sobre Memoria de Procesos
- **Consulta VQL Típica**:
  ```sql
  SELECT * FROM foreach(
      row = { SELECT * FROM pslist() WHERE Name =~ "javaw|minecraft" },
      query = {
          SELECT Pid, Name, String FROM yara(
              accessor = "process",
              files = format(format = "/%d", args = Pid),
              context = 20,
              rules = 'rule DetectCheat { strings: $a = "Wrath" wide nocase $b = "Aimbot" nocase condition: any of them }'
          )
      }
  )
  ```
- **Qué detecta**:
  - Lee el espacio de memoria virtual del proceso objetivo (`javaw.exe`).
  - Si existen cadenas en texto plano (en Unicode o ASCII) o firmas de bytes correspondientes a funciones de trampas, YARA extrae el offset de memoria, el PID y 20 bytes de contexto circundante como prueba forense irrefutable.

### B. Caza de Código Inyectado y Páginas Anómalas
- **Artefactos VQL**: `Windows.Memory.InjectedCode`, `Windows.Detection.ProcessHollowing`.
- **Heurística de Detección**:
  - Velociraptor consulta los Virtual Address Descriptors (VAD) del proceso.
  - Identifica páginas con atributos sospechosos:
    1. Páginas con protección `PAGE_EXECUTE_READWRITE` (RWX).
    2. Páginas de tipo `MEM_PRIVATE` (memoria asignada dinámicamente con `VirtualAllocEx`) que contienen código ejecutable sin estar respaldadas por un archivo en disco (`SEC_IMAGE`).
    3. Módulos que no figuran en `PEB.Ldr` pero que contienen cabeceras PE válidas (`0x5A4D` / `MZ`).

### C. Análisis de Sockets y Conexiones Volátiles
- **Artefacto VQL**: `Windows.Network.Netstat`.
- **Qué audita**:
  - Conexiones TCP/UDP activas por proceso.
  - Sockets abiertos por el proceso del juego hacia servidores web, paneles de autenticación de cheats o servidores de comandos.
