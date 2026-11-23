# Wrath Source — Proyecto Unificado

Código fuente completo y unificado del cliente **Wrath** (Loader + Payload + Bypass) para Minecraft 1.21.0 – 1.21.11 (Fabric).

## Estructura del Código Fuente (`wrath/`)

- **`bypass/`**: Primitivas compartidas de sigilo, evasión de escáneres de memoria (`StringScrub.h`), hilo remoto oculto (`NtCreateThreadEx` con `HIDE_FROM_DEBUGGER`), objeto centinela mimetizado (`WilStaging_02`) e higiene de disco (`StealthDefs.h`).
- **`client/`**: Código fuente completo de `WrathPayload.dll` (módulos de combate, movimiento, renderizado, utilidades, assets embebidos en `.rdata`, hooks JVMTI/MinHook y ClickGUI).
- **`docs/`**: Inventario de módulos, historial de correcciones y documentación de perfiles.
- **`loader/`**: Código fuente de `WrathLoader.exe` (interfaz DirectX 11 / FreeType con manifiesto UAC de Administrador integrado e inyector sigiloso).

## Compilación (Release x64)

Desde la carpeta `wrath/`:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --target WrathLoader
```

La compilación despliega automáticamente en `../Injeccion/`:
- `WrathLoader.exe`
- `WrathPayload.dll`
- `assets/`

## Verificación

```powershell
cmake -S . -B build -A x64 -DVANTA_BUILD_TESTS=ON
cmake --build build --config Release --target WrathLoader wrath_regression wrath_packet_regression
cmake --build build --config Release --target wrath_jnihook_shutdown_regression wrath_storage_safety
ctest --test-dir build -C Release --output-on-failure
```

Las pruebas cubren prioridades y cancelación de eventos, concurrencia,
inicialización única, persistencia de perfiles y hooks de paquetes en una JVM
real con clases de prueba. No sustituyen las pruebas dentro de Minecraft.

La prueba de paquetes cubre el cierre con callbacks activos, el reenvío durante
la parada y un segundo intento de desenganche. La prueba aislada de JNIHook
inyecta fallos de restauración y de cierre del entorno: intenta todas las
clases y conserva referencias y cachés cuando hace falta reintentar.

La carga de perfiles valida los tipos antes de aplicar cambios y restaura la
instantánea anterior si falla su aplicación. El autoguardado conserva y
reintenta las escrituras fallidas cada segundo mientras el cliente siga activo;
una instantánea posterior reemplaza a la pendiente. El cambio de perfil se
cancela si no se pueden guardar los cambios pendientes. Las pruebas incluyen
archivos bloqueados y un fallo de aplicación simulado; la restauración no puede
garantizar revertir efectos externos de callbacks ni fallos de memoria/proceso.

El login de prueba está conservado para conectar autenticación posteriormente.
Los eventos de paquetes usan JNIHook y no dependen del shim Java opcional, que
continúa desactivado. NoFall expone únicamente su modo implementado, Packet.

La descarga conserva el DLL si falla la restauración o quedan callbacks/hilos
activos. La eliminación de buffers y archivos propios no garantiza ausencia
de rastros ni eficacia frente a detectores.

