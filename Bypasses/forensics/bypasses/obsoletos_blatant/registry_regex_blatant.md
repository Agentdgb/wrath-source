# Bypass Obsoleto / Blatant: Rutas en AppData/Temp y Borrado Naive

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Los errores más comunes cometidos por desarrolladores de cheats públicos que son atrapados en segundos mediante búsquedas por expresiones regulares en Registry Explorer.

---

## 1. Descargar y Ejecutar el Loader en `AppData`, `Temp` o `Public`
- **Técnica errónea**: Colocar el loader en:
  `C:\Users\Admin\AppData\Local\Temp\loader.exe`
  o `C:\Users\Public\Downloads\cheat.exe`.
- **Por qué falla ante la búsqueda por Regex (Baneo Inmediato)**:
  - El analista carga todas las colmenas y ejecuta el filtro:
    ```regex
    ^C:\\Users\\.+\\AppData\\.+$
    ```
  - Windows registra esa ruta en múltiples ubicaciones del registro automáticamente:
    - En `MuiCache` con el nombre de la ventana o descripción.
    - En `BAM` (`Services\bam\State\UserSettings\<SID>`) con el timestamp de ejecución.
    - En `AppCompatFlags\Compatibility Assistant\Store`.
  - La regex extrae todas estas entradas en una sola tabla. Ver un `.exe` no reconocido en `AppData\Local\Temp` en un reporte de Timeline Explorer es una de las mayores firmas de trampas.

---

## 2. Borrado Simple de Valores sin Sobrescritura de Celdas (Slack Space Leak)
- **Técnica errónea**:
  Borrar la ruta del registro con `RegDeleteValueW()` o comandos `reg delete`.
- **Por qué falla**:
  - Registry Explorer y `rla.exe` escanean el espacio no asignado (slack space) de los bloques `hbin`.
  - La cadena que contiene la ruta (`C:\...\loader.exe`) sigue existiendo físicamente en los bytes no sobreescritos de la celda.
  - La regex `^([a-zA-Z]:\\.+)\?$` coincide tanto con valores activos como con valores desasignados en slack space, recuperando la ruta borrada con su nombre y fecha original.

---

## 3. Crear Claves de Persistencia en `Run` o `RunOnce`
- **Técnica errónea**:
  Configurar el cheat para que inicie con Windows agregando un valor en `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.
- **Por qué falla**:
  - Es el primer lugar donde mira cualquier analista forense.
  - La regex de persistencia (`Run`, `RunOnce`) lo resalta en la primera fila del reporte de Registry Explorer.
