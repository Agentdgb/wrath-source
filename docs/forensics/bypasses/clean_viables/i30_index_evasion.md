# Bypass Limpio: Evasión de Índices $i30 en NTFS

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass asegura que ningún archivo del cliente o del loader deje entradas inactivas ni residuos en los bloques `$INDEX_ALLOCATION` de las carpetas de usuario del sistema host (`C:\`).

---

## 1. Vector Principal: Aislamiento por Contenedor VHD Volátil (M08)
*Ubicación en el código:* `Bypasses/tools/setup_vhd_environment.ps1` y `teardown_vhd_environment.ps1`.

1. **Localidad del Atributo `$i30`**:
   - Cada sistema de archivos NTFS mantiene sus propios árboles `$INDEX_ALLOCATION` exclusivamente para sus propios directorios.
   - Si un binario se crea en `C:\Users\Admin\Downloads`, el índice `$i30` de `Downloads` en `C:\` recibe la entrada.
2. **Solución Wrath**:
   - `WrathLoader.exe` y sus dependencias **nunca se colocan en carpetas estándar del host** (`Downloads`, `Desktop`, `Temp`).
   - El despliegue se realiza dentro del volumen virtual VHD montado sin letra de unidad (`NON_PNP`).
   - Las operaciones de archivo ocurren exclusivamente dentro del árbol de directorios del VHD (`Backup_2026.vhd`).
   - **Resultado en INDXRipper**:
     - Al ejecutar `INDXRipper.exe //./C: --deleted-dirs output.csv` sobre el disco `C:\`:
     - El índice `$i30` de las carpetas del host **no contiene una sola entrada** relacionada con Wrath.
     - En `C:\` solo existe la entrada del contenedor general `Backup_2026.vhd` en su carpeta legítima de señuelo (`C:\System_Backups`).
3. Al desmontar el VHD, el volumen virtual se desconecta y no deja fragmentos en los índices del volumen host.

---

## 2. Inyección Directa en Memoria (Zero-File Payload)
*Ubicación en el código:* `Bypasses/injector/ManualMap.h`.

1. `WrathPayload.dll` nunca se materializa como archivo independiente en el sistema de archivos NTFS.
2. Se inyecta directamente desde buffers cifrados en memoria hacia `javaw.exe`.
3. Al no existir llamada a `CreateFile` para la DLL, **ningún directorio de Windows crea un nodo `$i30` para el payload**.

---

## 3. Sobrescritura de Slack Space en Nodos de Índice (Medida de Contingencia)
Si por alguna razón fuera necesario colocar un binario temporal en una carpeta del host antes de una sesión:
1. **El Problema**: Borrar el archivo deja la entrada en el slack space del bloque `$INDEX_ALLOCATION` de la carpeta padre.
2. **Mitigación Limpia**:
   - En esa misma carpeta se crean secuencialmente múltiples archivos señuelo legítimos con nombres genéricos (e.g., `temp_cache_001.tmp` ... `temp_cache_200.tmp`).
   - La inserción secuencial de entradas en el árbol B-Tree del directorio fuerza a NTFS a reorganizar los nodos y reasignar los bloques de 4096 bytes de `$INDEX_ALLOCATION`, sobrescribiendo físicamente los bytes residuales del slack space previo.
   - Posteriormente, se eliminan los temporales legítimos de forma controlada o se consolidan.
