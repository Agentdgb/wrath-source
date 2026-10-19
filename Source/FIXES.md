# Correcciones de estabilidad y persistencia

## ThrowPot, configs portables y coste por frame

- ThrowPot conserva Healing, Strength y Speed; Throws tiene solamente Randomize. Look Down y sus cambios de pitch están retirados.
- Healing usa una poción si su curación estimada completa la vida; en caso contrario selecciona hasta dos. Ambas pueden completar la vida, sin reserva obligatoria ni Health Threshold. Con 1,5 corazones y Healing II selecciona dos; con 8 corazones selecciona una. Comprueba la vida antes de cada lanzamiento, omite la segunda si ya está curado y no amplía ni reinicia la secuencia. Strength y Speed usan una sola poción.
- La cantidad depende de la curación estimada; curaciones externas, efectos de servidor y distancia de impacto pueden cambiar el resultado real.
- Rename edita el nombre desde el campo superior y conserva los cambios del perfil activo. Invalida guardados antiguos, impide sobrescribir otro perfil y admite cambios de mayúsculas.
- Share copia un código WRATH1 autocontenido al portapapeles, con identificador aleatorio y checksum para detectar copias dañadas. Import code lee ese código, valida su tamaño y valores y crea un perfil con nombre único. Funciona sin servidor; el código es largo y no está cifrado.
- Caché de clases JNI con referencias locales para sus consumidores y pausa de 500 ms entre búsquedas fallidas; evita repetir búsquedas por todos los classloaders en cada frame.
- Los logs descartados en Release ya no formatean sus argumentos. Las activaciones temporales Once/Hold no provocan autosaves ni se restauran como acciones pendientes.
- Pruebas añadidas para planes de curación, códigos aleatorios/dañados, nombres reservados, renombrado con autosave pendiente, importación sin sobrescritura y propiedad de referencias JNI.

La mejora de FPS requiere medición dentro de Minecraft: estas correcciones eliminan trabajo repetido, pero no verifican que todos los tirones desaparezcan.

- Login de prueba conservado, con sus campos y flujo de acceso.
- Estado compartido del loader sincronizado; el fallo de inyección permite reintentar.
- Eventos ordenados desde su primera publicación, con observadores después de la cancelación y snapshots reutilizados.
- Autosave: snapshot en el hilo de interfaz, escritura en worker, debounce real y cierre sin mantener el bloqueo de estado durante el join.
- Escrituras completas comprobadas, reemplazo mediante archivo temporal, rechazo de nombres que escapan del directorio y secuencias que impiden guardar snapshots antiguos sobre versiones nuevas.
- Eliminación de un perfil invalida trabajos pendientes para impedir que vuelva a aparecer.
- Restauración central de métodos Java antes de liberar cachés. Los callbacks nativos están contados y la descarga falla de forma conservadora si no terminan.
- Los hooks de OpenGL, cursor y ventana comprueban el drenaje antes de liberar trampolines o referencias.
- Inicialización cancelable durante descarga y publicación atómica del contexto JVM.
- Referencias JNI de NoJumpDelay y NoHitDelay liberadas al desactivar/destruir; referencias locales y caché compartida retiradas en su ciclo de vida.
- Borrado de strings limitado a sus caracteres válidos; guardado retirado de DllMain.
- Eventos de paquetes implementados mediante JNIHook. Firmas comprobadas contra [Yarn 1.21.11](https://maven.fabricmc.net/docs/yarn-1.21.11%2Bbuild.4/net/minecraft/network/ClientConnection.html).
- Opciones sin implementación de NoFall retiradas. NoFall y ChatSpy se desactivan con una explicación si no se pueden instalar los hooks de paquetes.
- Presupuestos de escaneo con reloj monotónico preciso. El escaneo parcial conserva un cursor; el procesamiento por chunks comprueba el presupuesto entre chunks completos y puede excederlo durante un chunk.
- Paso de distribución de CMake corregido: copia directa y errores visibles, sin comandos PowerShell con separación incorrecta.
- Pruebas de regresión activables con VANTA_BUILD_TESTS.

## Alcance de la comprobación

Las pruebas usan la JVM del JDK encontrado por CMake y clases artificiales con
las firmas de red. No se ha ejecutado el cliente dentro de Minecraft ni probado
la matriz completa de versiones, GPU, mods o servidores. La implementación
opcional del shim/transformador Java sigue desactivada; los eventos de paquetes
funcionan por una ruta independiente.

Persisten advertencias del compilador en componentes heredados y dependencias.
La limpieza no es una garantía de eliminación de rastros o de evasión.
