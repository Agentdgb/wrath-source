# Bypass Obsoleto / Blatant: Borrado o Corrupción de AppCompatCache

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Métodos destructivos que resultan en baneo inmediato durante cualquier análisis forense con `AppCompatCacheParser` o herramientas de screenshare.

---

## 1. Borrado Directo del Valor `AppCompatCache`
- **Técnica errónea**: Ejecutar comandos como:
  ```cmd
  reg delete "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\AppCompatCache" /v "AppCompatCache" /f
  ```
- **Por qué falla ante AppCompatCacheParser (Baneo Inmediato)**:
  - Todo sistema operativo Windows funcional acumula cientos de kilobytes de datos en `AppCompatCache` registrando la actividad normal del sistema.
  - Al ejecutar `AppCompatCacheParser.exe -f ...`, si el valor no existe o arroja error de lectura, el parser y los scripts de screenshare (Echo, Paladin, Ocean) emiten la alerta:
    **`[CRITICAL] AppCompatCache Missing or Deleted - Evidence Tampering Detected`**.
  - No existe ninguna justificación legítima para que un usuario estándar tenga eliminado este valor del registro.

---

## 2. Relleno con Ceros (Zeroing) del Valor Binario
- **Técnica errónea**: Sobrescribir el contenido binario de `AppCompatCache` con ceros (`0x00`) o truncarlo a 0 bytes.
- **Por qué falla**:
  - `AppCompatCache` tiene una cabecera binaria obligatoria que incluye un magic number (e.g. `0x10adc0de` o estructura específica de versión del kernel), recuento de entradas y tamaño total.
  - Al recibir un buffer en ceros, `AppCompatCacheParser` falla con una excepción de cabecera inválida (*Corrupt/Malformed ShimCache Header*).
  - Los inspectores catalogan la corrupción deliberada del valor como sabotaje directo de las herramientas de auditoría.

---

## 3. Borrar el Ejecutable Inmediatamente Después de Iniciar
- **Técnica errónea**: Iniciar `cheat.exe` y llamar inmediatamente a `del cheat.exe`.
- **Por qué falla**:
  - El kernel ya registró la ruta `C:\...\cheat.exe` en la memoria del ShimCache con `Executed = True`.
  - Cuando el analista forense ejecuta `AppCompatCacheParser`, la herramienta evalúa si el archivo existe físicamente en el disco.
  - Si `Executed = True` pero el archivo fue eliminado, la fila se resalta automáticamente en color rojo brillante como **`Missing Executed Binary`**, evidenciando que el usuario intentó destruir la prueba de la ejecución.
