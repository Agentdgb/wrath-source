# Bypass Limpio: Evasión de la Cadena de Evidencia de Existencia

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass asegura la ruptura matemática de la cadena de correlación forense: si los componentes del cliente nunca nacen en el volumen host y nunca interactúan con el shell gráfico, la totalidad de los 12 artefactos permanece en un estado nulo de detección.

---

## 1. Principio Fundamental: Cero Génesis en el Volumen Host (VHD M08)
*Ubicación en el código:* `Bypasses/tools/setup_vhd_environment.ps1` y `teardown_vhd_environment.ps1`.

Para que los artefactos de existencia registren un archivo, dicho archivo debe tocar el sistema de archivos del disco o interactuar con el Explorador de Windows:

1. **Aislamiento por VHD Volátil Desacoplado**:
   - Todo el ciclo de vida del loader y sus dependencias ocurre dentro del disco virtual VHD (`Backup_2026.vhd`) montado sin letra de unidad (`NON_PNP`).
2. **Impacto en los 12 Artefactos del Host**:
   - **Recycle Bin**: Nunca se envía nada a la papelera; no existen archivos `$I` ni `$R` en `C:\$Recycle.Bin\<SID>`.
   - **LNK Files**: Cero archivos `.lnk` en `%APPDATA%\...\Recent`.
   - **Jump Lists**: Cero streams `.automaticDestinations-ms` asociados a Wrath.
   - **Shellbags**: El usuario nunca abre el Explorador de Windows para explorar la carpeta privada del VHD (`BagMRU` y `Bags` no contienen la ruta).
   - **Open/Save MRU & RecentDocs**: Cero uso de diálogos comunes `comdlg32`.
   - **Prefetch**: Gestionado atómicamente mediante `restore_prefetch()` (ver sección 2).
   - **AmCache**: Polimorfismo con `build_salt.h` y recursos de `version.rc` mimetizados.
   - **ShimCache**: Payload en DLL (las DLLs no entran en ShimCache); loader mimetizado sin borrado anticipado.
   - **Browser History**: Cero apertura de archivos locales en navegadores (cero URLs `file:///`).
   - **Windows Search (`Windows.edb`)**: `WSearch` no indexa directorios dentro de puntos de montaje sin letra de unidad.
   - **Activity History (`ActivitiesCache.db`)**: Wrath no es una aplicación UWP/Modern con identificador de paquete; Windows Timeline no genera tarjetas de actividad.

---

## 2. Teardown Forense Atómico Obligatorio
*Ubicación en el código:* `Bypasses/trace/teardown.cpp` (`wrath::trace::teardown`).

Cuando la sesión de inyección finaliza, la suite ejecuta una secuencia de restauración en orden inverso obligatorio:

```cpp
teardown_result_t teardown(const snapshot_t& snap) {
    teardown_result_t res{};
    res.prefetch_restored = restore_prefetch(snap); // 1. Desvinculación POSIX / Restauración
    res.bam_restored      = restore_bam(snap);      // 2. Anti-Slack space scrubbing
    res.pca_restored      = restore_pca(snap);      // 3. Restauración de registros PCA
    res.stamps_restored   = restore_stamps(snap);   // 4. Sincronización $SI y $FN
    return res;
}
```

- Cada paso garantiza que los metadatos residuales sean devueltos a su estado previo al milisegundo exacto, impidiendo que el analista detecte saltos en las marcas de tiempo entre artefactos.

---

## 3. Preservación de Servicios de Auditoría (`WSearch`, `SysMain`, `DPS`)
- Todos los servicios responsables de la telemetría y recolección de artefactos permanecen en estado `Running`.
- Al auditar el sistema en vivo, el revisor comprueba que los servicios están funcionando con normalidad, validando la legitimidad del entorno.
