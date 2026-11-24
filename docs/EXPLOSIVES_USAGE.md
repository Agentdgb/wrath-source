# Crystal y Anchor: configuración sencilla

## Uso

Elige el módulo que necesitas:

- **Fast Crystal**: coloca y rompe cristales. **Mode: Auto** combina ambas
  acciones; Place y Break actúan sobre lo que tienes bajo la mira.
- **Anchor Macro**: coloca, carga y detona un anchor.
- **Safe Anchor**: añade y confirma el bloque de protección antes de cargar
  y detonar. Aim Mode: Auto apunta automáticamente; Manual utiliza tu mira.

Empieza con **Velocidad: Normal**. Si todavía resulta rápido, usa **Lenta**.
El ajuste controla cada paso y también la pausa entre ciclos; ya no hay que
coordinar Min Delay y Max Delay.

| Velocidad | Mínimo entre acciones | Retraso de ciclo |
| --- | ---: | ---: |
| Lenta | 200 ms | 500 ms |
| Normal | 125 ms | 250 ms |
| Rapida | 75 ms | 100 ms |

Son tiempos mínimos. Los ticks, la rotación, la visibilidad y las confirmaciones
del mundo pueden alargarlos. La macro no omite una confirmación para cumplirlos.

**Avanzado** muestra Auto Swap, Only Own Anchors, Target Radius, Material Reserve,
Shield Block, Only On Ground o Pause On Kill, según el módulo. Sus valores
guardados siguen funcionando aunque el panel avanzado esté cerrado.

## Correcciones de comportamiento

- Crystal continúa las búsquedas limitadas desde la última posición examinada;
  una base válida después de los primeros 48 candidatos ya puede encontrarse.
  Las bases ocupadas se descartan antes de elegir y almacenar la posición.
- Con un rival seleccionado, Crystal compara las posiciones por su estimación
  de daño en vez de imponer siempre la base que está bajo la mira.
- Cada colocación, carga y detonación comprueba el objeto actual de la hotbar.
  Un cambio de inventario o de Auto Swap cancela el plan incompatible y lo
  vuelve a calcular, conservando la referencia al anchor del ciclo.
- El slot original queda ligado al mundo y al inventario. Una selección manual
  posterior no se sobrescribe con una restauración antigua.
- Los tres módulos comparten una reserva por tick para evitar acciones
  duplicadas o intercaladas, incluso si una llamada de interacción falla.
- Abrir el menú, perder el foco o activar Freecam descarta las acciones
  pendientes. Pause On Kill también alcanza a los ataques de Crystal.
- Un anchor con cuatro cargas ya no queda bloqueado solo porque haya glowstone
  en la mano secundaria.
- La activación de un solo clic termina la secuencia completa: en Anchor espera
  la desaparición del bloque tras detonar; en Crystal automático termina tras
  romper el cristal. Colocar o cargar no desactiva prematuramente la macro.

## Referencias revisadas

Se consultaron los fuentes locales de Vape en
`NetworkBridge/VapeV4.21-main`: AutoAnchor, AnchorMacroState, CrystalAura y sus
submódulos de colocación. Se adaptó la separación de fases y temporizadores.
También se revisó CrystalAuto de GodWeer, especialmente los cambios y la
restauración de slot. Las rutas JNI y de interacción siguen siendo las del
cliente actual; no se trasladaron llamadas de otra versión del juego.

## Validación y límites

`wrath_explosive_modules_regression` compila el código real de los módulos y
sus ajustes contra un host aislado. Comprueba los tres paneles, el valor Normal
por defecto, la ausencia de los retrasos antiguos, el acceso a Avanzado,
el descarte de planes al pausar, la finalización de un solo clic y la cancelación
de una restauración entre mundos. También prueba las políticas utilizadas por producción para ritmo,
objetos válidos, reserva de tick, slots y continuación de búsquedas.

```powershell
ctest --test-dir build-wrath -C Release -R '^wrath_explosive_modules_regression$' --output-on-failure
```

La prueba pasa en Release. No reproduce un servidor, la latencia real ni toda
la colocación y detonación dentro de Minecraft. La selección por daño sigue
usando la estimación existente basada en distancia, no una simulación completa
de exposición, armadura o daño propio. No se certifica compatibilidad con todos
los anticheats.

Backup previo: `Backups/explosives_before_fixes_20261031_211720`.
