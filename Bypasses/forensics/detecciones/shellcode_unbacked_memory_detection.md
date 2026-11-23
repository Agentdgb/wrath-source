# Detección Forense: Shellcode y Memoria Mapeada Manualmente en RAM

## 1. Visión General del Vector
Cuando un payload opera en RAM sin respaldar su código en disco (inyección directa, manual mapping, shellcode), los indicadores de detección más concluyentes provienen de las propiedades estructurales de las regiones de memoria que ocupa y de los atributos de los hilos que lo ejecutan.

A diferencia del código legítimo, que se carga siempre desde imágenes PE validadas (`MEM_IMAGE`), el código inyectado no dispone de un archivo PE en disco del que derivar sus permisos, dejando huellas inequívocas en la tabla VAD del kernel y en las estructuras PEB/LDR del proceso.

---

## 2. Los Tres Indicadores Primarios

### A. Memoria Privada Ejecutable Sin Respaldo en Disco (`MEM_PRIVATE` + `PAGE_EXECUTE_*`)

La regla forense fundamental:

> *Todo código legítimo que se ejecuta en un proceso se carga desde un archivo PE en disco como una sección mapeada (`MEM_IMAGE`). Cualquier región `MEM_PRIVATE` con permisos de ejecución es anómala por definición.*

Clasificación de tipos de región y su significado:

| `Type` | `Protect` | Veredicto |
|---|---|---|
| `MEM_IMAGE` | `PAGE_EXECUTE_READ` | Normal (código de módulo legítimo) |
| `MEM_MAPPED` | `PAGE_READONLY` | Normal (archivo mapeado, e.g. datos) |
| `MEM_PRIVATE` | `PAGE_EXECUTE_READWRITE` | **INDICADOR CRÍTICO** (RWX en memoria privada) |
| `MEM_PRIVATE` | `PAGE_EXECUTE_READ` | Sospechoso (shellcode post-hardening) |

**Herramientas de Detección**:
* **Process Hacker / System Informer** → `Properties → Memory` → filtrar por `MEM_PRIVATE` + permisos de ejecución.
* **pe-sieve** (hasherezade): Escana todo el espacio de direcciones del proceso y reporta regiones `MEM_PRIVATE` ejecutables sin respaldo en módulo.
* **Moneta**: Detecta páginas `MEM_PRIVATE` RWX dentro de rangos de dirección de módulos conocidos.
* **YARA sobre volcados de proceso** (regla universal):
  ```
  condition: memory_region.type == MEM_PRIVATE and memory_region.protect & PAGE_EXECUTE != 0
  ```

### B. Puntos de Entrada de Hilo en Espacio Sin Módulo (Orphan Thread Entry Points)

Cada hilo del sistema tiene un punto de entrada (`StartAddress`) registrado en el ETHREAD del kernel. Para hilos legítimos:
* `StartAddress` resuelve con `LdrFindEntryForAddress()` a un módulo listado en `PEB->Ldr->InLoadOrderModuleList`.
* El módulo correspondiente tiene un archivo PE en disco con la misma imagen.

Para hilos de shellcode o manual mapping sin enlace al Ldr:
* `LdrFindEntryForAddress(StartAddress)` devuelve `NULL` → **hilo huérfano**.
* System Informer muestra `StartAddress` como una dirección hexadecimal desnuda (`0x7FF8DEADBEEF`) sin nombre de módulo.
* **pe-sieve** y **Volatility3** (`windows.thrdscan`) reportan estos hilos como `Suspicious Thread`.

### C. Redirección de Funciones y Hooks Detectables

El payload que opera en RAM normalmente necesita hookar funciones del proceso huésped:

* **VMT Hooks**: El puntero en la tabla virtual de un objeto apunta a una región `MEM_PRIVATE` en lugar de a código del módulo original.
* **Trampoline Hooks** (`jmp`/`call` al inicio del prólogo): Los primeros 5-14 bytes de una función se sobreescriben. `pe-sieve` los detecta comparando el contenido en memoria del módulo con el archivo en disco (hash section-by-section).
* **IAT Hooks**: La Import Address Table del módulo tiene entradas que apuntan fuera del rango de direcciones del módulo exportador original.

---

## 3. Herramientas de Análisis Combinado

### pe-sieve + hollows_hunter
`hollows_hunter.exe /pname javaw.exe /imp 3 /shellc /iat` escanea el proceso completo y genera un reporte estructurado con:
* Regiones sospechosas (unbacked executable memory).
* Hilos huérfanos.
* Hooks en módulos cargados.
* Implantes de shellcode en regiones de trabajo.

### Volatility3 (Análisis de Volcado en Frío)
Plugins relevantes:
* `windows.vadinfo`: Lista todas las regiones VAD del proceso con tipo, permisos y archivo de respaldo.
* `windows.thrdscan`: Reporta hilos cuyo `StartAddress` cae en regiones sin módulo.
* `windows.malfind`: Heurística clásica: busca `MEM_PRIVATE` + `PAGE_EXECUTE_*` + cabecera MZ/PE.
* `windows.dlllist` correlacionado con `windows.modules`: Detecta módulos en memoria sin correspondencia en disco.
