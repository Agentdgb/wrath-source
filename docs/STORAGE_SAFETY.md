# Seguridad de perfiles y avisos de descarga

## Cambios

- Se retiró la limpieza automática de archivos anteriores al iniciar y cerrar, incluida la eliminación recursiva de la antigua carpeta de AppData.
- Si Windows no puede resolver LocalAppData, no se utiliza el directorio actual. El guardado queda desactivado y se muestra un aviso.
- Los perfiles se almacenan en `%LOCALAPPDATA%\WrathClient`, una carpeta específica del cliente.
- Los perfiles reconocidos de la ubicación anterior se copian sin sobrescribir los actuales ni borrar originales. Una marca de migración impide volver a importar perfiles purgados.
- La purga explícita valida la ruta contra la ubicación autorizada, rechaza rutas relativas, raíces y enlaces/redirecciones, y conserva archivos ajenos.
- La purga no borra carpetas recursivamente. Elimina únicamente archivos JSON marcados como perfiles del cliente; comprueba la propiedad y solicita la eliminación sobre el mismo archivo abierto.
- Los errores de borrado se comunican. Los fallos de descarga muestran un aviso de Windows también en Release, sin depender del logger de diagnóstico.
- La creación de hilos informa de éxito o fallo. La descarga conserva el DLL si no puede completar un cierre seguro o iniciar el hilo final.
- El estado final interno es `ReleaseRequested`: solicitar la liberación no se presenta como confirmación externa de que el DLL ya desapareció.

## Verificación

La prueba `wrath_storage_safety` comprueba rutas vacías, relativas, raíces,
carpetas ajenas, archivos sin marca, migración sin sobrescritura, preservación
de originales, ausencia de reimportación tras purga y fallos al crear el hilo.
También comprueba una carpeta redirigida si Windows permite crear el enlace
o la junction de prueba; si no, valida una junction existente de Windows en
modo de solo lectura, sin enumerar ni borrar su destino. Los archivos bloqueados deben conservarse y producir
un error observable.

```powershell
cmake -S . -B build -A x64 -DVANTA_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

El login de prueba y el código de FPS no se modificaron como parte de estos
cambios. Los avisos dentro de una sesión real de Minecraft requieren una
comprobación manual; las pruebas no inyectan el cliente en el juego.

Validación realizada: compilación Release x64 y las tres suites aprobadas
(`wrath_regression`, `wrath_packet_regression`, `wrath_storage_safety`).
