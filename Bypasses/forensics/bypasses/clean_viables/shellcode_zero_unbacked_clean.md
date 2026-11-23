# Evasión Limpia: Zero-Unbacked Memory, SEC_IMAGE y Hilos Respaldados (Wrath / VANTA)

## 1. Principio: Toda Memoria Ejecutable del Payload es MEM_IMAGE

La regla inviolable del mapeo Wrath:

> *"El payload nunca ocupa páginas `MEM_PRIVATE` con permisos de ejecución. Toda región ejecutable del payload es de tipo `MEM_IMAGE`, con permisos `PAGE_EXECUTE_READ`, indistinguible estructuralmente de cualquier DLL del sistema operativo cargada formalmente."*

Esto elimina el indicador primario de shellcode (`MEM_PRIVATE` + `PAGE_EXECUTE_*`) que activan `pe-sieve`, `hollows_hunter`, `Volatility3 windows.malfind` y los escáneres de memoria YARA.

---

## 2. Mecanismo de Mapeo SEC_IMAGE (Anti-Shellcode)

### Por Qué SEC_IMAGE Elimina los Tres Indicadores Primarios

La clave es la combinación de `NtCreateSection(SEC_IMAGE)` + `NtMapViewOfSection`:

```cpp
// Wrath: Creación de sección con tipo SEC_IMAGE desde hFileTx (Transacted NTFS)
HANDLE hSection = nullptr;
NtCreateSection(
    &hSection,
    SECTION_MAP_READ | SECTION_MAP_EXECUTE | SECTION_MAP_WRITE,
    nullptr,
    nullptr,
    PAGE_EXECUTE_READ,  // Permisos de la sección - READ+EXECUTE, nunca WRITE
    SEC_IMAGE,          // Tipo IMAGE — no SEC_RESERVE, no SEC_COMMIT
    hFileTx             // Archivo PE válido (aunque sea transaccional)
);

// La región en el proceso destino aparece con:
// Type    = MEM_IMAGE         ← No MEM_PRIVATE
// Protect = PAGE_EXECUTE_READ ← No RWX
// State   = MEM_COMMIT        ← Normal
NtMapViewOfSection(hSection, hTarget, &base, ...);
```

| Indicador de Shellcode | Comportamiento Wrath |
|---|---|
| `Type = MEM_PRIVATE` | ❌ No. `Type = MEM_IMAGE`. |
| `Protect = PAGE_EXECUTE_READWRITE` | ❌ No. `Protect = PAGE_EXECUTE_READ`. |
| Hilo con `StartAddress` en `MEM_PRIVATE` | ❌ No. Entry point en región `MEM_IMAGE`. |
| `pe-sieve`: Sección discrepante vs disco | Parcial: TxF revertido = sin archivo en disco, pero no es `MEM_PRIVATE`. |

---

## 3. Scrubbing de Cadenas en Memoria (Anti-YARA / bstrings)

La segunda capa de protección es el cifrado en tiempo de compilación de todas las cadenas del payload (`enc.h`) y el saneado inmediato tras el uso (`StringScrub.h`):

```cpp
// Uso típico en el payload (dentro de javaw.exe)
{
    char resolved[32];
    auto s = WRATH_STACK_STRING("TargetField");
    s.decrypt(resolved);

    // Uso puntual de la cadena
    resolve_field(resolved);

    // Scrub inmediato: RtlSecureZeroMemory sobre la copia en pila
    s.scrub(resolved);
}
// Stack frame liberado → la cadena desaparece del dump RAM
```

Un volcado de la memoria de `javaw.exe` con `WinPmem`, `bstrings` o `MagnetProcessCapture` **no encuentra cadenas de control completas en ninguna región del Heap ni de la pila** del payload.

---

## 4. Prohibición Total de Hooks Detectables

Los hooks que Wrath realiza dentro del proceso de la JVM son **exclusivamente via JNI native method registration** (vinculación de métodos nativos del payload a métodos declarados como `native` en el Fabric mod), sin modificar:
* Tablas de métodos virtuales (`VMT`) del cliente del juego.
* IAT/EAT de módulos de la JVM (`jvm.dll`, `java.dll`).
* Instrucciones de prólogo de funciones (`jmp`/`call` a memoria externa).

No hay ninguna región `MEM_PRIVATE` ejecutable, ningún hilo huérfano, y ninguna función cuyo prólogo sea discrepante respecto a su imagen en disco. `pe-sieve /iat` y `pe-sieve /shellc` sobre `javaw.exe` no reportan anomalías.
