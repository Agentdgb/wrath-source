# Organización del proyecto

La estructura sigue el criterio de Vanta: `Injeccion/` contiene la distribución,
`wrath/` el código fuente y `Backups/` las copias y el material retirado.
El repositorio Git permanece en la raíz y conserva su historial.

- `Source/` pasó a llamarse `wrath/`; conserva `client/`, `loader/`, `bypass/`
  y las dependencias necesarias. No se modificaron los módulos del cliente.
- Los binarios de la raíz eran más recientes que los de `Wrath Updated/`.
  Los actuales se trasladaron a `Injeccion/` junto con sus recursos y servidor.
- Los documentos auxiliares se agruparon en `wrath/docs/`.
- La compilación despliega en `Injeccion/`. Los lanzadores utilizan su propia
  carpeta como directorio de trabajo.
- `Backups/Wrath_antes_de_ordenar_2026-10-23.tar.gz` conserva la estructura
  anterior, incluidos los cambios locales y archivos no registrados en Git.
  Excluye `.git`, la caché `Source/build` y las cachés de Python.
- `Backups/Retirado/` contiene `.fix-build`, `tools_audit`, la distribución
  antigua `Wrath Updated`, la caché de compilación anterior, la caché de Python
  y `ManualMap.h.orig`. El control automático rechazó su borrado; se retiraron
  de las carpetas activas mediante movimientos, sin eliminarlos del disco.

El archivo de compilación anterior apuntaba a otra ubicación del equipo.
Después del traslado debe generarse una caché nueva con los comandos del README.

## Retirada de VMProtect

Se retiraron los includes del SDK, las marcas de regiones protegidas, la
comprobación de depurador de VMProtect y las rutas del SDK en CMake.
El SDK, los proyectos de protección y las dos copias de su DLL quedaron fuera
del proyecto activo, en `Backups/VMP_retirado/`, junto con los fuentes anteriores.
La compilación normal ya utilizaba funciones vacías del SDK; ahora tampoco
necesita ese encabezado ni sus bibliotecas.

## Verificación

- Configuración nueva de CMake y compilación Release x64 de WrathLoader y WrathPayload completadas.
- Los dos binarios generados se desplegaron en Injeccion y sus hashes coinciden con build/dist.
- 1.497 archivos de la fuente comparados con el backup, sin cambios inesperados.
- No se ejecutaron los lanzadores ni se probó una sesión dentro de Minecraft.

Verificación posterior a la retirada: compilación Release completada; WrathLoader.exe
y WrathPayload.dll desplegados en Injeccion, sin importaciones ni secciones VMP.
No se ejecutó una sesión de Minecraft.
