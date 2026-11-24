# Bypass Limpio: Evasión de MRU, Jump Lists y RecentApps

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass garantiza que ningún comando, ruta o archivo del cliente quede registrado en los artefactos de actividad de usuario de Windows Shell, sin requerir el borrado masivo de historiales que levanta banderas rojas.

---

## 1. Vector Principal: Cero Interacción con Windows Shell
*Regla operativa de Wrath / VANTA:*

1. **Nunca Utilizar el Diálogo "Ejecutar" (`Win + R`)**:
   - `RunMRU` solo se alimenta cuando un usuario abre físicamente la ventana `Win + R` y tipea un comando.
   - `WrathLoader.exe` se inicia directamente mediante procesos programáticos (e.g. llamada nativa `CreateProcessW` con flags `CREATE_NO_WINDOW` o atajos sin interacción del shell de explorer).
   - **Resultado en `Analyze-MRURegistry.ps1`**: En `HKCU\...\Explorer\RunMRU` **no se genera absolutamente ninguna entrada**. Cero registros del loader o de scripts auxiliares.
2. **Cero Navegación Manual en Explorer (TypedPaths)**:
   - Los archivos de Wrath residen en el VHD o rutas de despliegue automatizadas (`build_out`, `Injeccion`).
   - El usuario nunca tipea manualmente la ruta en la barra de direcciones del Explorador de Windows, manteniendo la clave `TypedPaths` inmaculada.

---

## 2. Prevención Total en Jump Lists
*Mecanismo del Contenedor VHD y Loader:*

1. **Aislamiento de Extensiones**:
   - `WrathLoader.exe` y `WrathPayload.dll` no asocian tipos de archivo en el sistema operativo (no registran verbos `shell\open\command` en el registro).
   - No se abren mediante aplicaciones registradas en la barra de tareas (como Notepad o editores de texto que generen entradas en `%APPDATA%\Microsoft\Windows\Recent\AutomaticDestinations`).
2. **Entorno Desacoplado**:
   - Al no generarse interacciones de tipo "Abrir con..." ni anclajes en la barra de tareas, Windows no crea ningún stream `.automaticDestinations-ms` relacionado con Wrath.
   - Las Jump Lists legítimas del sistema (Chrome, Discord, Explorador) conservan su historial natural de navegación y documentos sin verse alteradas ni reseteadas.

---

## 3. Prevención en RecentApps
1. `HKCU\Software\Microsoft\Windows\CurrentVersion\Search\RecentApps` solo indexa aplicaciones invocadas mediante el menú inicio / búsqueda de Windows (`SearchUI` / `StartMenuExperienceHost`) o aplicaciones empaquetadas MSIX/UWP.
2. Un binario Win32 nativo ejecutado de forma directa o desde un proceso de servicio/lanzador nunca es agregado a esta clave.

---

## 4. Normalización / Reemplazo Atómico en Caso de Accidente
Si por error el usuario llegó a tipear algo en `Win + R`:
1. **Nunca borrar toda la clave `RunMRU`**.
2. **Sustitución Coherente**:
   - Se identifica el valor alfabético específico (e.g., valor `b` que contenía el comando).
   - Se sobrescribe el valor `b` con un comando de Windows totalmente inocuo y habitual (e.g., `notepad\1` o `taskmgr\1`) mediante `RegSetValueExW`.
   - Se mantiene intacta la cadena de orden `MRUList` (e.g., `"abcdef\0"`).
   - Se fuerza `RegFlushKey` para actualizar la colmena.
   - Para el script `Analyze-MRURegistry.ps1`, la entrada simplemente refleja que el usuario abrió el Administrador de Tareas o el Bloc de Notas.
