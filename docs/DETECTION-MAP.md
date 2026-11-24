# Detection Map

Índice de `docs/forensics/`. Cada entrada es una técnica de detección o evasión
documentada; el código de este repositorio se mide contra ella.

## Categorías

### Evasiones viables (`forensics/bypasses/clean_viables/`)
Técnicas que no destruyen evidencia de forma flagrante. Incluye:
- `shellcode_zero_unbacked_clean.md` — memoria privada sin archivo (lo que hace el manual mapping).
- `module_overloading_sec_image_evasion.md` — VAD `MEM_IMAGE`.
- `fileless_evasion.md`, `pre_launch_zero_exe_clean.md` — sin exe en sesión.
- `hiding_strings_native_clean.md` — limpieza de strings.
- `srum_evasion.md`, `registry_evasion.md`, `mft_evasion.md` — artefactos de disco/registro.

### Evasiones obsoletas / blatantes (`forensics/bypasses/obsoletos_blatant/`)
Técnicas que generan evidencia de anti-forense. El repositorio **no las usa**:
journal tampering, borrado de event logs, servicios suspendidos, BYOVD, VM.

### Detecciones (`forensics/detecciones/`)
- `shellcode_unbacked_memory_detection.md` — VAD privado + PE (loader invariant).
- `usermode_hiders_and_injection.md` — PEB vs VAD, hilos sin módulo.
- `kernel_live_dump.md` — command lines en kernel memory.
- `volatility_memory_forensics.md`, `velociraptor_vql_yara.md` — captura de memoria.
- `srum_srumecmd.md`, `amcache_amcacheparser.md`, `mft_mftecmd.md` — disco/registro.

## Estado de este código frente al mapa

| Superficie | Estado |
|---|---|
| VAD privado con PE | Presente (manual mapping). Medido por `wrath_probe`. |
| PEB vs VAD | El módulo se desvincula (`unlink`) si `Options::unlink`. |
| Hilos sin módulo | Start address en región privada. |
| Strings | Limpieza parcial; `PayloadInit`/`JNI_OnLoad` se barren. |
| Disco/registro | Sin limpieza agresiva (decisión: no subir la escalera anti-forense). |

El detalle completo de cada técnica está en los archivos indexados arriba.
