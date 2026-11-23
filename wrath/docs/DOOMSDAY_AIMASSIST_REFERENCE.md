# Referencia de AimAssist: Doomsday 0.9.10

Se examino `C:/Users/Admin/Downloads/doomsday_source` sin ejecutar Doomsday
ni sus agentes. El objetivo fue adaptar el estilo solicitado, no sustituir
la integracion JNI que ya funcionaba en este cliente.

## Resultado y limites

Se recupero la clase de configuracion de AimAssist y ClickAimAssist:
`dpkg/qOeMBAAAAAA`. Su constructor declara los modos **Wind** y **Simple**, y
controles como Speed, Sensitivity, Speed 1, Speed 2, Prediction, Distance,
Aim position, Fov, Mouse block, Only weapon, Vertical aim y Focus mode.

La clase comun delega el giro en adaptadores de version referenciados como
qyeMBAAAAAA, qzeMBAAAAAA y varias clases q*eNBAAAAAA. Sus cuerpos no aparecen
entre los archivos de clase estandar recuperados. Esa cobertura parcial no
permite afirmar que no existan en otra representacion del paquete; tampoco
permite atribuir a Doomsday una formula de movimiento que no se ha verificado.

La opcion **Aim style** de nuestro cliente es una adaptacion propia:

- **Simple**, elegido por el usuario y seleccionado por defecto: conserva la
  respuesta proporcional directa del AimAssist nuevo que ya funcionaba.
- **Wind**, alternativa opcional: amortigua la velocidad al entrar y al cambiar
  de direccion. Su constante de 35-60 ms es nuestra calibracion, no un valor
  recuperado de Doomsday. No se agrega ruido aleatorio ni se permite que la
  inercia mueva la mira en sentido contrario al objetivo.

Ambos usan Horizontal speed, Horizontal strength y el mismo Focus. La seleccion
de rival, las pausas, la sensibilidad real y las zonas de apuntado se conservan.
No se incorporan autoclick, CPS ni otras funciones ajenas al apuntado.

## Trazabilidad

| Artefacto | Identidad y transformacion |
| --- | --- |
| dist/doomsday.jar | SHA-256 00023869fb623480a99384f4d487fdbc3a15bdf92971870e3c1f700270a11bce; inventariado con REA, 332 archivos y 3 directorios. |
| src/main/resources/64FV7P4H2NO7Q | SHA-256 837b6653b185c1ecafb0e43f60b26eeb137b5d7a8b268573502977cadb5268b6. Cabecera: version numerica 6058 y texto 0.9.10 con XOR 0x47. |
| Imagen de cliente derivada | Se omiten 64 bytes de cabecera, se aplica XOR 0x57 y se descomprime XZ. SHA-256 88fc1044d3bf8e451db2b95db9645841398a3d78c07712b16fedfec4d2cab61d, 13.362.273 bytes. |
| Clases recuperadas | 2.716 class files validos por estructura JVM, con offsets, longitudes, nombres y hashes individuales. No equivale a cobertura de todas las representaciones internas del paquete. |
| Tabla de textos | Offset 2.733.239, 4.811 entradas. Longitudes u16, flags y bytes invertidos con XOR 0xFF. Se relaciona el orden de campos de qGeFBAAAAAA con etiquetas como AimAssist y ClickAimAssist. |
| Clase de configuracion | Offset 7.954.197, longitud 12.620, SHA-256 43a7bd8e7fa78d691c45d4a76dc194956603bd6904cac0b9e62636f21587cb78. |

Las clases auxiliares se recuperaron del recurso net/java/e mediante el algoritmo
XOR y LZMA descrito por las fuentes del inyector. Se inspeccionaron con javap y
[CFR 0.152](https://www.benf.org/other/cfr/), descargado del autor y comprobado
contra su checksum publicado. No se ejecuto codigo del cliente analizado.

La evidencia completa y los derivados estan en
`C:/Users/Admin/.codex/tmp/doomsday-analysis-20261101_183657`:
`artifact-evidence.json`, `client-class-inventory.json`, `decoded-ui-strings.json`,
`field-labels.json`, `aimassist-labelled.txt` y las clases originales recuperadas.
El archivo labelled es una ayuda de lectura derivada, no codigo Java compilable.

## Respaldo y pruebas

Respaldo anterior a los estilos:
`Backups/aimassist_before_doomsday_style_20261101_192230`.

Las pruebas del controlador cubren ambos estilos a distintos FPS, entrada gradual,
cambio de direccion, limite del punto de apuntado, prioridad del raton y fuerza
horizontal cero. La prueba con JVM real cubre el cambio de estilo conservando
el rival y la recuperacion de errores de camara sin ocultarlos en cuadros sin giro.
No se ha establecido paridad dinamica con Doomsday ni se ha probado la sensacion
final en una partida real.
