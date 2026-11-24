# Bypass Obsoleto / Blatant: Suspensión de Hilos de Servicios y Bloqueo de Herencia

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Técnicas surgidas entre 2021 y 2022 que en la actualidad son detectadas de forma inmediata por cualquier analista o herramienta de screenshare moderna.

---

## 1. Suspender Hilos de `PcaSvc`, `SysMain` o `DPS` con Scripts o Process Hacker
- **Técnica errónea**:
  Usar scripts o Process Hacker para localizar los hilos pertenecientes a `pcasvc.dll` o `sechost.dll` dentro del proceso `svchost.exe` y suspenderlos (`NtSuspendThread`) para que no registren ejecuciones.
- **Por qué falla ante los analistas (Baneo Inmediato)**:
  1. **Visibilidad en System Informer**:
     - El revisor inspecciona la lista de hilos de `svchost.exe`.
     - El hilo suspendido resalta en color rojo o con el estado explícito **`Suspended`**.
     - Ningún hilo de un servicio nativo de Windows permanece suspendido en condiciones normales.
  2. **La Prueba del Prefetch**:
     - El revisor ejecuta `notepad.exe` o `calc.exe` y comprueba `C:\Windows\Prefetch\`.
     - Al ver que `SysMain` figura como `Running` pero no se crea el archivo `.pf` para la nueva aplicación, se concluye **Sabotaje Interno de Servicios (Ban Permanente)**.

---

## 2. Quitar Herencia y Denegar Permisos de Escritura en Claves de Registro
- **Técnica errónea**:
  Abrir `regedit`, ir a `HKLM\SYSTEM\CurrentControlSet\Services\bam\State\UserSettings\<SID>` y desmarcar "Heredar permisos", quitando los permisos de escritura a `SYSTEM` para que Windows no pueda registrar ejecutables en BAM.
- **Por qué falla ante los escáneres**:
  - Herramientas automatizadas de screenshare (Echo, Paladin) auditan los descriptores de seguridad (DACL) de las claves de telemetría.
  - Si la clave tiene la herencia deshabilitada o devuelve `ACCESS_DENIED` al intentar escribir una prueba, el escáner emite la alerta:
    **`[CRITICAL] Registry Inheritance Disabled / Permissions Tampered on BAM`**.
  - Esta alteración premeditada es tratada como prueba concluyente de trampa.
