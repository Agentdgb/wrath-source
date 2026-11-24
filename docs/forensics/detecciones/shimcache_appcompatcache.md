# Detección Forense: ShimCache (AppCompatCache) y AppCompatCacheParser

## 1. Identificación y Alcance del Artefacto
- **Artefacto**: ShimCache / Application Compatibility Cache (`AppCompatCache`).
- **Ubicación en el Registro**:
  `HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Session Manager\AppCompatCache` (Valor binario: `AppCompatCache`).
- **Objetivo**: Componente del subsistema de compatibilidad de aplicaciones de Windows diseñado para recordar metadatos de ejecutables y optimizar la aplicación de "shims" del sistema operativo.
- **Herramientas de Análisis**:
  - `AppCompatCacheParser.exe` (Eric Zimmerman Tools)
  - `Timeline Explorer` / `EZViewer`

---

## 2. Metadatos Extraídos por AppCompatCacheParser
El formato del binario varía según la versión del kernel (Windows 10/11 utiliza típicamente la estructura versión 4 de 520 bytes por entrada con cabecera de control):

1. **Ruta Completa del Ejecutable (`Path`)**:
   - Cadena UTF-16 con la ubicación absoluta donde residía el binario cuando fue evaluado/ejecutado.
   - **Persistencia Post-Borrado**: Aunque el archivo ejecutable haya sido eliminado del disco, su registro en ShimCache **permanece intacto** en la caché binaria.
2. **Fecha de Última Modificación (`LastModifiedTime`)**:
   - Marca de tiempo del archivo en disco en el instante en que el sistema de compatibilidad lo analizó.
3. **Bandera de Ejecución (`Executed`)**:
   - Indica si el archivo fue efectivamente ejecutado por el usuario/sistema (`True`) o si fue simplemente indexado/explorado (`False`).
4. **Posición / Orden de Inserción**:
   - Las entradas se organizan en una tabla de tipo LRU (Least Recently Used) o secuencial. Permite reconstruir qué programas se ejecutaron consecutivamente.

---

## 3. Dinámica de Almacenamiento (Memoria vs Disco)
- Durante la sesión activa de Windows, las nuevas entradas se mantienen en la memoria del kernel (`win32k.sys` / `csrss.exe`).
- El volcado completo al valor del registro `AppCompatCache` en el archivo `SYSTEM` de disco se realiza:
  - Al apagar o reiniciar el sistema operativo de forma limpia.
  - Al crearse un punto de restauración o snapshot de volumen (VSS).
  - Al ejecutarse comandos como `reg save HKLM\SYSTEM`, el kernel vuelca las transacciones pendientes.

---

## 4. Heurísticas y Reglas de Flag en Screenshares (Echo, Paladin, Ocean)
- **Flag 1 — Presencia de Binarios No Estándar**: Rutas en `AppData\Local\Temp`, `Downloads`, escritorios o rutas temporales con nombres de loaders conocidos, inyectores o cadenas sospechosas.
- **Flag 2 — Archivos Ejecutados pero Ausentes en Disco**: Entradas en ShimCache con `Executed = True` cuyo archivo ya no existe en el sistema de archivos (evidencia directa de que el ejecutable fue borrado).
- **Flag 3 — Correlación Temporal**: Ejecutable registrado en ShimCache con marca temporal coincidente con el inicio de la sesión de juego o torneo.
- **Flag 4 — Valor AppCompatCache Vacío o Corrupto**: Si el valor binario fue eliminado o tiene tamaño anómalo (menor a varios kilobytes), los analizadores arrojan advertencia crítica inmediata (*ShimCache Cleared/Tampered*).
