# Matrix 7.25.0: diagnóstico de rotación

Fecha: 2026-10-31.

## Evidencia disponible

La captura del usuario identifica Matrix 7.25.0 y muestra `KILLAURA (ka.abt.syn)`.
Otra captura menciona un patrón de apuntado similar a aimbot/sync. Eso acredita
la alerta, pero no demuestra un falso positivo ni identifica por sí solo el
algoritmo que la produjo. No se dispone de una captura de control sin asistencia.

La carpeta local Anticheats contiene AGC, Demon, Grim, Intave, Overflow, Spartan,
SpookyAC, Verus y Vulcan. No contiene el código del check de Matrix.

Se consultó el [repositorio público de Matrix](https://github.com/re-ovo/Matrix-Issues)
y su [configuración de checks](https://github.com/re-ovo/Matrix-Issues/blob/master/checks.yml).
Publica opciones separadas para aimbot sync, sensibilidad y aceleración; no
publica ahí la implementación de `ka.abt.syn`. No se interpreta una configuración
como si fuera el código del detector ni se vincula automáticamente el master
del repositorio con la versión concreta 7.25.0.

Como contraste local se leyeron Intave RotationSensitivityHeuristic y Vulcan
AimH/AimI. Sus comprobaciones de sensibilidad sirven para revisar la coherencia
de los pasos de cámara, no para certificar el resultado de Matrix.

## Defectos reproducibles y corrección

Se comprobó el bytecode local de Minecraft 1.21.11: Mouse calcula la escala en
double con constantes float promovidas, y Entity.changeLookDirection convierte
el delta escalado a float antes de multiplicarlo por 0,15f.

El cliente anterior reducía la sensibilidad a float y calculaba la escala en
float. Además, devolvía 0,5 si fallaba la lectura del valor real y escribía
directamente los campos de rotación.

La nueva ruta conserva la precisión original, usa el getter de sensibilidad
cuando está disponible, y llama a la función de rotación de Minecraft. Esa
función conserva el tratamiento del pitch, los ángulos anteriores y pasajeros.
Una sensibilidad no disponible detiene la asistencia; no inventa un valor.
La cuantización verifica el movimiento físico resultante después del redondeo.
No se cambiaron las ganancias, las velocidades, la memoria del rival ni los
ajustes de usuario. No se modificaron configuraciones ni alertas del anticheat.

## Pruebas

`wrath_aimassist_mouse_regression` compara con una referencia Java independiente
las 201 sensibilidades entre 0 y 1 en pasos de 0,005 y 257 desplazamientos enteros
entre -128 y 128. Son 51.657 comparaciones exactas de bits: la ruta nueva coincide
en todas; el cálculo anterior difiere en 35.432. Esto mide paridad aritmética,
no 35.432 detecciones de Matrix.

El mismo ejecutable prueba el código JNI real contra una fixture Java: lectura
sin pérdida de precisión, llamada a la función de giro, límites del pitch actual
y anterior, entrada desactivada y ausencia de opciones. Las fixtures representan
las rutas probadas; no son una ejecución completa del cliente de Minecraft.

También pasan las regresiones de movimiento, foco e integración del modo Regular:

```powershell
ctest --test-dir build-wrath -C Release -R '^wrath_aimassist_(mouse|motion|focus|regular)_regression$' --output-on-failure
```

## Validación pendiente

No se ha ejecutado Matrix 7.25.0 contra esta compilación. La corrección elimina
defectos de compatibilidad confirmados; no demuestra que desaparezca ka.abt.syn
ni convierte una detección de apuntado automático en falso positivo.
La siguiente comprobación necesita una prueba real con la misma configuración
del servidor y una comparación con la asistencia desactivada.

Backup anterior: `Backups/aimassist_before_rotation_parity_20261031_205210`.
