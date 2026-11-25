# Wrath Client (Dt Core) - Documentación Técnica y Arquitectura

Bienvenido a la documentación técnica del cliente in-game **Wrath** (`dt_core` / `WrathPayload.dll`). Este componente es una biblioteca dinámica nativa escrita en C++20 que se ejecuta directamente dentro del proceso de Java (`javaw.exe`) de Minecraft (enfocado en Fabric 1.21+).

---

## 1. Arquitectura y Árbol de Directorios (`src/`)

```
wrath/client/src/
├── core/               # Núcleo del cliente, configuración y ciclo de vida
│   ├── config/         # Almacenamiento y serialización de perfiles (JSON)
│   ├── event/          # Bus de eventos interno desacoplado (EventBus)
│   ├── input/          # Captura y despacho de entradas de teclado/ratón
│   ├── keybind/        # Mapeo y disparo de atajos de teclado
│   ├── module/         # Interfaz base Module y gestor ModuleManager
│   ├── profile/        # Estructura de datos de perfiles y ajustes
│   ├── setting/        # Sistema de tipos para sliders, toggles y selectores
│   ├── social/         # Gestión de lista de amigos y objetivos
│   ├── ClientState.h   # Estado de autorización y flags globales
│   ├── HeartbeatWatchdog.cpp / .h # Verificación remota cada 45s y neutralización
│   ├── NativeCallbacks.h # Delegados C++ a eventos del juego
│   └── UnloadSafety.h  # Control atómico de apagado seguro
│
├── entry/              # Punto de entrada y bootstrap
│   ├── DllMain.cpp     # DLL_PROCESS_ATTACH asíncrono
│   ├── Bootstrap.cpp   # Hilo desacoplado de inicialización
│   ├── DtInit.cpp   # Orquestador del arranque de subsistemas
│   ├── Shutdown.cpp    # Procedimiento de apagado limpio
│   └── version.rc      # Metadatos del recurso PE de Windows
│
├── hook/               # Capa de intercepción y hooks
│   ├── HookManager.cpp # Gestor centralizado de hooks nativos (MinHook)
│   ├── PacketEvents.cpp# Intercepción de paquetes de red cliente-servidor
│   ├── ClassFileLoadHook.cpp # Intercepción de clases cargadas por la JVM
│   └── Retransform.cpp # Retransformación dinámica de bytecode
│
├── jvm/                # Puente JVM y funciones JNI
│   ├── JvmContext.cpp  # Localización y attach a JavaVM / JNIEnv
│   ├── JniHelper.cpp   # Helpers para resolución de métodos, campos y firmas
│   ├── JniRef.cpp      # Punteros RAII para referencias locales/globales de JNI
│   ├── HandleTable.cpp # Tabla de objetos vivos de Java en memoria
│   ├── LoaderProbe.cpp # Inspección de classloaders en tiempo de ejecución
│   └── VersionDetect.cpp # Detección dinámica de la versión de Minecraft
│
├── mappings/           # Resolución de nombres ofuscados
│   ├── MappingRegistry.cpp # Diccionario en .rdata (Yarn / Intermediary)
│   └── generated/      # Tablas estáticas constexpr por versión
│
├── modules/            # Módulos de funcionalidad y combate
│   ├── combat/         # AimAssist, SilentAim, TriggerBot, AutoTotem, Backtrack
│   ├── movement/       # Sprint, NoFall, AutoJumpReset, NoJumpDelay
│   ├── render/         # Esp, BlockEsp, Chams, HitFx, Outline, Radar, SwordReplace
│   ├── visuals/        # FullBright, ItemShader, Freecam, WorldTweaks, Spectate
│   ├── utilities/      # AutoTool, FastPlace, ItemScroller
│   ├── misc/           # ChatSpy, StreamProof
│   └── shared/         # Rutinas matemáticas comunes y helpers de renderizado
│
├── render/ & vgui/     # Capa visual e interfaz gráfica
│   ├── bridge.cc       # Enlace con wglSwapBuffers y captura de contexto GL
│   ├── main.cc         # Renderizado de la ventana ImGui
│   └── vgui.h          # API pública para controlar la visibilidad del menú
│
├── platform/           # Wrappers de hilos y funciones Win32
└── util/               # Logging formateado (VLOG), matemáticas y hashing
```

---

## 2. Flujo de Vida del Cliente (Lifecycle)

1. **Inyección en RAM:** El inyector (`WrathLoader.exe`) reserva memoria en `javaw.exe` y ejecuta `ManualMapInMemory()`.
2. **Arranque Asíncrono:** `DllMain` recibe `DLL_PROCESS_ATTACH` y crea inmediatamente un hilo de bootstrap separado para no congelar el proceso de Java.
3. **Enganche a OpenGL:** El subsistema `vgui` localiza la ventana de Minecraft y desvía la función `wglSwapBuffers` para renderizar el menú ImGui en cada fotograma del juego.
4. **Inspección de la JVM:** Se localiza el puntero `JavaVM`, se asocia el hilo a `JNIEnv` y `VersionDetect` identifica la versión del juego para activar la tabla de mappings correspondiente.
5. **Supervisión Zero-Trust (Watchdog):** `HeartbeatWatchdog` lee la sección compartida de memoria salteada (`Local\WinSync_<HASH>`) para extraer el token de sesión y comienza el latido periódico cada 45 segundos contra el servidor.

---

## 3. Reglas de Estabilidad y Seguridad (Políticas del Código)

### A. Política de Neutralización Segura (*Safe Neutralization*)
* **Regla estricta:** **NUNCA descargar la DLL (`FreeLibraryAndExitThread`) ni desmontar los hooks de OpenGL (`wglSwapBuffers`) con el juego en ejecución.**
* *Motivo:* Los motores de renderizado modernos (como **Sodium** o **Iris** en Fabric) mantienen pipelines multihilo continuos. Si se desengancha OpenGL o se libera la memoria en caliente, el hilo de renderizado dereferencia punteros nulos (`0xc0000005`) provocando el crash inmediato de Minecraft.
* *Acción correcta:* Cuando una sesión expira, se cancela o falla la autenticación, se invoca [`HeartbeatWatchdog::disableClientSafely()`](file:///c:/Users/Admin/Desktop/Wrath%20Source/wrath/client/src/core/HeartbeatWatchdog.cpp#L128-L145), la cual:
  1. Ejecuta `ModuleManager::panic()` apagando todos los módulos activos.
  2. Activa las banderas atómicas de shutdown (`g_shutting_down`) para que los hooks JNI no toquen nada.
  3. Cierra la interfaz visual.
  4. La DLL se mantiene dormida en memoria de forma pasiva, garantizando 100% de estabilidad en el juego.

### B. Canales de Memoria Compartida (IPC)
* Todo intercambio de estado con el Loader utiliza identificadores salteados generados mediante `wrath::bypass::heartbeatSectionNameForPid(pid)`.
* No se deben usar nombres estáticos ni cadenas que contengan palabras clave como `wrath`, `jvm` o `hack`.

---

## 4. Guía para Agregar un Nuevo Módulo

1. **Crear los archivos:** Ubicar el nuevo módulo en la subcarpeta correspondiente dentro de `src/modules/<categoria>/`.
2. **Heredar de `Module`:**
   ```cpp
   #include "core/module/Module.h"

   namespace dt::modules {
       class MiNuevoModulo : public Module {
       public:
           MiNuevoModulo() : Module("MiNuevoModulo", Category::Combat, "Descripción del módulo") {}
           void onEnable() override { /* Activación */ }
           void onDisable() override { /* Limpieza */ }
           void onTick() override { /* Lógica de juego */ }
       };
   }
   ```
3. **Registrar en `ModuleManager`:** Agregar la instancia dentro de `ModuleManager::registerModules()` en `src/core/module/ModuleManager.cpp`.
4. **Compilación automática:** Gracias a `file(GLOB_RECURSE)` en `CMakeLists.txt`, cualquier nuevo `.cpp` dentro de `src/modules/` es incluido automáticamente en la siguiente compilación.

