# Detección Forense: Anomalía Temporal Pre-Juego (Pre-Launch Timeline Clustering)

## 1. Visión General del Vector
En las auditorías forenses competitivas modernas (Echo, Paladin, Ocean, Avenge), los evaluadores aplican análisis de **Correlación Temporal Contextual** (*Contextual Timeline Clustering*). 

No se limitan a buscar binarios con nombres delatores (`cheat.exe`, `vape.exe`); analizan la **secuencia cronológica de eventos en los 5 a 10 minutos previos a la creación del proceso del juego (`javaw.exe`)**.

---

## 2. Cómo Detectan la Anomalía los Checkers

### Herramientas Utilizadas
* **`Nirsoft/LastActivityView`**: Consolida eventos de `UserAssist`, `Prefetch`, `OpenSaveMRU`, `Shellbags` y registros del sistema en una línea de tiempo ordenada por segundos.
* **`Zimmerman/AppCompatCacheParser` (ShimCache)**: Muestra el orden de ejecución de binarios y su última modificación.
* **`Spokwn/PcaSvcExecuted.exe`**: Rastrea la base de datos de PCA buscando ejecutables arrancados recientemente.
* **`BAM` (Background Activity Moderator)**: Timestamp de 64 bits de la última ejecución de cada `.exe`.

### El Patrón Anómalo Delator
Cuando un usuario usa un loader (incluso si está camuflado como `DisplayTunerService.exe`, `Calculator.exe` o `svchost.exe` local):
```
[18:24:12] - C:\...\DisplayTunerService.exe (Ejecutado)
[18:24:45] - C:\...\MinecraftLauncher.exe (Ejecutado)
[18:25:02] - C:\...\javaw.exe (Proceso iniciado)
```
El evaluador aísla la ventana previa a `javaw.exe` y pregunta:
> *"¿Por qué abriste este ejecutable 40 segundos antes de iniciar Minecraft?"*

Aunque el ejecutable tenga iconos legítimos o recursos clonados de Microsoft, **su mera presencia en la ventana temporal previa a la sesión de juego destruye la coartada y focaliza la investigación forense** sobre ese binario específico, sus claves en BAM y su memoria.
