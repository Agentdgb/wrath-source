# Técnicas Flagrantes: Ejecutables Camuflados en la Ventana Pre-Juego

## 1. Visión General de Técnicas Descartadas
Muchos loaders intentan camuflarse cambiando su nombre a ejecutables comunes del sistema (`calculator.exe`, `DisplayTuner.exe`, `audiocontrol.exe`, `svchost.exe` en carpetas locales, `cleaner.exe`) y pidiéndole al usuario que los abra antes de abrir el juego.

---

## 2. Por Qué Fracasan Inevitablemente

### 1. El Salto Temporal Específico
Un evaluador de screenshare no necesita saber si `DisplayTuner.exe` es un cheat o una utilidad real. La pregunta que formula es:
> *"Tienes 400 ejecutables en tu PC. ¿Por qué justamente 30 segundos antes de abrir Minecraft ejecutaste este programa?"*

Si el usuario responde *"es para calibrar el monitor"*, el checker le pide abrirlo en pantalla, revisar sus configuraciones de pantalla o examinar su memoria. Si el archivo ya fue borrado, `DPSAnalyzer` lo delata en `dps-full-notfound-executables.txt`.

### 2. Evidencia en `UserAssist` (ROT13)
El doble clic en el explorador de archivos actualiza la clave del registro:
```
HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\UserAssist\{CEBFF5CD-...}\Count
```
Esta clave registra:
* La ruta del ejecutable.
* El contador de ejecuciones.
* La fecha y hora exacta de la última ejecución.
Incluso si el archivo fue eliminado, la entrada en `UserAssist` persiste, demostrando que el usuario abrió un programa manualmente antes de jugar.

### 3. Falta de Coartada Contextual
En los deportes electrónicos y servidores competitivos, los registros de actividad antes de los torneos o partidas clasificatorias se auditan estrictamente. Un clúster de ejecución inusual previo a la partida constituye motivo de suspensión por sospecha fundada bajo las reglas de la mayoría de ligas de screensharing.
