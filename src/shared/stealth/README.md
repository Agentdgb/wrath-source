# Utilidades compartidas y límites de limpieza

Esta carpeta reúne utilidades existentes que comparten loader y cliente.

`StringScrub.h` borra los caracteres válidos de buffers propios y libera su
almacenamiento. No escribe fuera de `size()` y no garantiza que desaparezcan
copias anteriores u otros rastros del proceso o sistema.

`StealthDefs.h` conserva las definiciones y funciones existentes. Su presencia
no demuestra eficacia frente a detectores ni anticheats.

La descarga restaura los métodos Java antes de retirar sus cachés y comprueba
los callbacks activos antes de liberar trampolines. Si falla la restauración,
el contexto GL o el drenaje, conserva el DLL y los recursos todavía necesarios.
No debe describirse como una autodestrucción completa garantizada.

El login sigue siendo una interfaz de prueba para integrar autenticación real
posteriormente. Actualmente no comprueba usuario o contraseña.

Las correcciones y límites de validación se detallan en [FIXES.md](../../../docs/FIXES.md).