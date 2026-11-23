# Inventario del cliente

44 módulos registrados. Eliminados: NoFall, ChatSpy, 3D Character, TotemSpirit y, previamente, MotionBlur.

Los módulos están implementados; no todos han sido comprobados en una partida. Los cambios se verifican mediante compilación y pruebas automatizadas para el entorno 1.21.11.

## Combat (14)

| Módulo | Estado o condición |
|---|---|
| AimAssist | Implementado; validación en partida pendiente. |
| Anchor Macro | Implementado; validación en partida pendiente. |
| Anti Action | Implementado; validación en partida pendiente. |
| Auto Crystal | Implementado; validación en partida pendiente. |
| AutoTotem | Implementado; validación en partida pendiente. |
| Backtrack | Implementado; validación en partida pendiente. |
| FastPot | Implementado; validación en partida pendiente. |
| NoHitDelay | Implementado; validación en partida pendiente. |
| Refill | Implementado; validación en partida pendiente. |
| Safe Anchor | Implementado; validación en partida pendiente. |
| ShieldBreaker | Ajustes: OnlySwords, SwitchBack, ReturnDelay (50–500 ms, predeterminado 150), CheckFacing y AxeSelection. Mantiene visible el hacha antes de restaurar; respeta cambios manuales de ranura. Compatible con Anti Action y TriggerBot; validación en partida pendiente. |
| SilentAim | Implementado; validación en partida pendiente. |
| ThrowPot | Instant activo por defecto; sin espera añadida, incluso con velocidad antigua guardada. |
| TriggerBot | Implementado; validación en partida pendiente. |

## Movement (3)

| Módulo | Estado o condición |
|---|---|
| AutoJumpReset | Implementado; validación en partida pendiente. |
| NoJumpDelay | Implementado; validación en partida pendiente. |
| Sprint | Implementado; validación en partida pendiente. |

## Utilities (3)

| Módulo | Estado o condición |
|---|---|
| AutoTool | Implementado; validación en partida pendiente. |
| FastPlace | Implementado; validación en partida pendiente. |
| ItemScroller | Implementado; validación en partida pendiente. |

## Misc (2)

| Módulo | Estado o condición |
|---|---|
| SlowSwing | Implementado; validación en partida pendiente. |
| StreamProof | Implementado; validación en partida pendiente. |

## Visuals (22)

| Módulo | Estado o condición |
|---|---|
| ArrayList | Implementado; validación en partida pendiente. |
| Atmosphere | Parcial: falta el sistema separado de clima y bloom original. |
| BlockESP | Parcial: escaneo local disponible; caché por paquetes sin eventos. |
| Chams | Implementado; validación en partida pendiente. |
| DeathEffects | Implementado; validación en partida pendiente. |
| FallingPetals | Pétalos dentro del mundo, con profundidad. Normal: 160; Heavy: 320. |
| Freecam | Implementado; validación en partida pendiente. |
| FullBright | Implementado; validación en partida pendiente. |
| HitFX | Implementado; validación en partida pendiente. |
| ItemShader | Implementado; validación en partida pendiente. |
| Notifier | Implementado; validación en partida pendiente. |
| Outline | Implementado; validación en partida pendiente. |
| PlayerESP | Implementado; validación en partida pendiente. |
| Radar | Implementado; validación en partida pendiente. |
| Spotify | Necesita una sesión multimedia compatible en Windows. |
| SwingAnimation | Implementado; validación en partida pendiente. |
| SwordReplace | 20 cosméticos y vistas previas desde la DLL, sin paquete ni recarga. |
| Target HUD | Implementado; validación en partida pendiente. |
| TargetESP | Implementado; validación en partida pendiente. |
| Watermark | Implementado; validación en partida pendiente. |
| WorldParticles | Implementado; validación en partida pendiente. |
| WorldTweaks | Implementado; validación en partida pendiente. |

## Cambios recientes

ShieldBreaker usa como referencias locales Vape, GodWeer y Deslight, además de inspección estática de los JAR de Descargas. Se revisaron las rutas de escudo de Anubis, UtilityMod, Mochi, System Ready y Amethyst; Arizona y Clarity también contienen referencias a la función. Water y UtilityMod exponen la misma ruta de clase. Se conservan el ataque original, una sola animación y la restauración de la ranura; se comprueba la orientación horizontal del escudo. Las pruebas usan una JVM con clases de prueba, no sustituyen una partida real ni certifican anticheats.

FallingPetals se dibuja en coordenadas del mundo con caída, viento, giro, transparencia ordenada y profundidad; no se dibuja sobre el menú. ThrowPot conserva el máximo de dos usos por acción y la restauración del slot, con Instant activado por defecto. La llegada al servidor sigue dependiendo de Minecraft y la conexión.

Se retiraron los recursos y hooks exclusivos de personajes y espíritus. Los efectos restantes comparten un lector de recursos independiente del código de personajes. Las texturas y los eventos se limpian al desactivar, cambiar de mundo y descargar.

## Corrección del cierre con Sodium

Se retiró el hook de DeathEffects sobre EntityRenderer y se comparte el de
visibilidad del dispatcher con Chams. La prueba JVM reproduce el NoSuchMethodError
del camino antiguo y verifica el camino corregido sin modificar el renderizador
de Sodium. Queda pendiente la validación en una partida.
