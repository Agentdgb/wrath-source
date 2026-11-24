# Evasión Limpia: Cero Ejecución Pre-Juego (Zero Pre-Launch Footprint)

## 1. Principio Fundamental: Línea de Tiempo Pura
Para superar el análisis de *Timeline Clustering*, la línea de tiempo previa a la creación de `javaw.exe` debe contener **exclusivamente eventos ordinarios de un usuario que se dispone a jugar**:

```
[18:20:00] - Discord.exe (En ejecución continua desde el boot)
[18:24:00] - MinecraftLauncher.exe (Iniciado por el usuario)
[18:24:40] - javaw.exe (Proceso del juego creado)
```
**Cero ejecutables adicionales. Cero utilidades intermedias.**

---

## 2. Mecanismos de Ejecución que Respetan la Línea de Tiempo

Para que el cliente se inyecte en `javaw.exe` sin que aparezca ningún `.exe` previo en `LastActivityView`, `BAM` ni `Prefetch`:

### 1. Inyección Originada en Controladores de Hardware Preexistentes (Peripheral Bridge)
* El proceso disparador es el software oficial del ratón o teclado (`lghub_agent.exe`, `RazerSynapse.exe`, `iCUE.exe`).
* **Timestamp en la Línea de Tiempo**: Este software fue iniciado al encender la PC (`Boot Time`, por ejemplo a las `10:00 AM`).
* Cuando el usuario activa la combinación física en el ratón dentro de Minecraft, la llamada de memoria se ejecuta desde el hilo del driver que lleva **horas de actividad legítima continua**.
* **Resultado**: `LastActivityView` no muestra ningún programa ejecutado antes de Minecraft.

### 2. Gancho Nativo de Subsistema Gráfico (`SetWindowsHookEx`)
* Registrado en `user32.dll` sin crear procesos ejecutables nuevos.
* El código se despacha de forma reactiva en el propio hilo de la ventana de Minecraft cuando esta se crea.

### 3. Preservación del Estado Natural
* Ninguna entrada agregada a `UserAssist`.
* Ninguna clave de ejecución temporal creada en BAM.
* Ningún archivo `.pf` anómalo con fecha inmediatamente previa al juego.
