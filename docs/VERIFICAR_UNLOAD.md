# Verificar Unload conservando los datos

## Uso

1. Utiliza los binarios nuevos de `Injeccion/` en la próxima sesión.
2. Descarga Wrath con Unload. Los perfiles y datos se conservan, también si
   mantienes Shift. El cierre sigue guardando los cambios normales del perfil.
3. Mantén Minecraft abierto y ejecuta `Injeccion/verificar_unload.bat`.
   Si hay varios procesos Java, ejecuta el script con `-TargetPid` y su PID.

El observador está fuera del DLL. Solo enumera módulos y consulta metadatos de
páginas mediante `VirtualQueryEx`: no lee contenidos de memoria, no escribe en
Minecraft y no ejecuta código dentro de él.

## Qué se registra

Los registros explícitos se guardan en
`%LOCALAPPDATA%\WrathClient\diagnostics`:

- `load-PID.json`: último método que el loader informó como exitoso:
  LoadLibrary, TxF, module stomp o manual mapping. No se modificaron esas rutas.
- `module-PID.json`: base y tamaño de la asignación que contenía el cliente
  al iniciar y si aparecía registrado en el cargador de Windows.
- `unload-PID.json`: solicitud de descarga, sin presentarla como éxito.
- `check-PID-FECHA.json`: resultado de la comprobación externa.

Se incluye la fecha de creación del proceso para rechazar registros de un PID
reutilizado. La proximidad temporal del método informado es solo una pista;
no constituye un identificador inequívoco de ciclo de carga.

No se registran tokens, HWID, contenido de perfiles ni contenido de memoria.
Los informes permanecen en disco por diseño y no se ocultan ni se borran.
Si el cliente no puede escribir el registro, el observador informa que falta
la referencia inicial; no asume que la descarga tuvo éxito.

## Cómo interpretar el resultado

| Resultado | Interpretación |
| --- | --- |
| `original_region_confirmed_free` | La región registrada anteriormente está libre en el momento de consultar. |
| `allocated_or_reused` | Hay páginas asignadas en ese intervalo. Pueden ser el cliente o una reutilización; no es una confirmación de descarga. |
| `no_baseline` | Falta la referencia inicial, por ejemplo por usar una versión anterior o por fallar la escritura. |
| `query_failed` / `access_denied` | La consulta no pudo completarse. |
| `stale_or_invalid_baseline` | El registro no corresponde al proceso actual o es inválido. |
| `process_exited` | El proceso terminó; esto no verifica Unload manteniendo Minecraft abierto. |

Incluso una región libre no demuestra ausencia de otras copias, historial del
sistema o recursos ajenos a esa región. No se comprueba aquí la ausencia de
todos los callbacks o recursos gráficos. Tampoco se certifica legitimidad ante
detectores. La liberación del mapeo manual sigue sin estar implementada.

## Datos conservados

Unload no llama a la purga de perfiles. La variante antigua de la función que
recibía un argumento de purga lo ignora. Se conserva la carpeta existente de
perfiles; no se migró ni se modificó su ubicación. La base de datos del servidor,
su configuración y los datos del loader no se borran con esta revisión.

## Verificación realizada

- Pruebas de diagnóstico, rutina de cierre y seguridad de perfiles.
- Casos del observador para región libre, ocupada, parcialmente ocupada,
  consulta fallida y referencia inválida.
- Consulta externa real de una asignación liberada por un proceso de prueba,
  sin inyección, con resultado `original_region_confirmed_free`.
- Los nueve archivos de datos de la revisión previa continúan presentes.
  Un perfil fue actualizado durante la sesión del usuario y se conservó.

La escritura nativa al directorio predeterminado de AppData devolvió acceso
denegado en el proceso de prueba de este entorno. La escritura y lectura de
metadatos se verificaron en una carpeta de prueba dentro del proyecto. La
creación del registro por Minecraft aún debe comprobarse en una sesión nueva.
No se cambiaron permisos ni protecciones de Windows.
