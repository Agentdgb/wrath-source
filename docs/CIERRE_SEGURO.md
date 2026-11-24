# Revisión del cierre de Wrath

## 1. Ejecutar la rutina de cierre

Antes, `spawnReaper` normalmente ignoraba el callback recibido y programaba
`FreeLibrary` mediante un temporizador de 750 ms. La espera de trabajadores de
`reapAndFree` solo se ejecutaba en una ruta de respaldo.

Ahora se usa el mismo despachador de callback en producción y en las pruebas.
Esto permite que se ejecuten las comprobaciones existentes antes de solicitar
la liberación. Se rechazan callbacks y creadores de hilos nulos, y se conserva
el error de creación del hilo.

Beneficio: elimina una liberación basada únicamente en un retraso fijo.
Límite: las comprobaciones existentes siguen requiriendo validación en Minecraft;
el contador de trabajadores no certifica por sí solo todos los recursos del proceso.

## 2. Comunicar los fallos

Antes, `reportUnloadFailure` descartaba el motivo de error. Ahora conserva el
estado y código de error, y muestra el motivo en un aviso de Windows, también
en Release. No depende del logger ni de que el overlay siga funcionando.

Beneficio: un cierre incompleto deja una explicación visible.
Límite: el aviso es modal y debe cerrarse; no confirma la liberación de memoria.
Su presentación en pantalla completa aún requiere comprobación manual.

## 3. Comprobar el registro del DLL

Antes de llamar a `FreeLibraryAndExitThread`, se consulta si Windows reconoce
la dirección base como un módulo registrado. Se rechazan un valor nulo, el
ejecutable principal, direcciones interiores y asignaciones que no son módulos.
La consulta no incrementa el contador de referencias.

Beneficio: evita utilizar una dirección de memoria cualquiera como handle de DLL.
Límite: estar registrado es una condición necesaria, no una prueba de identidad,
propiedad o descarga completa. No valida todas las variantes de carga existentes.
Una imagen que falla esta comprobación queda detenida y residente, con un aviso.
No se añadió un mecanismo de liberación del mapeo manual.

## Verificación automática

`wrath_reaper_regression` ejecuta el callback real por la ruta predeterminada,
la ruta con creador personalizado, fallos de creación y parámetros inválidos.
También comprueba un DLL registrado, el ejecutable principal, un handle nulo y
una asignación privada. No inyecta ni descarga código en otro proceso.

Se ejecutan además `wrath_storage_safety` y
`wrath_jnihook_shutdown_regression` para revisar regresiones cercanas al cierre.

## Siguiente comprobación: una sesión real

Pendiente de realizar en Minecraft:

1. Registrar el método de carga utilizado y el estado antes de pedir Unload.
2. Confirmar que se restauran controles, renderizado y comportamiento del juego.
3. Comprobar el estado de los trabajadores y recursos después del cierre.
4. Distinguir una descarga solicitada de una liberación confirmada externamente.
5. Repetir el ciclo para detectar acumulación de recursos o fallos de estabilidad.

Estas comprobaciones evalúan el cierre y su estabilidad. No son una certificación
de indetectabilidad ni eliminan el historial del sistema.

La comprobación externa posterior y la política de conservación de datos se
describen en [VERIFICAR_UNLOAD.md](VERIFICAR_UNLOAD.md). Shift ya no solicita
una purga durante Unload.

Resultado de esta revisión: las tres pruebas indicadas pasaron y la compilación
Release x64 terminó correctamente. Los dos binarios se desplegaron en Injeccion
y se compararon con build/dist. No se probó una sesión dentro de Minecraft.
