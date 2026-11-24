# Bypass Obsoleto / Blatant: Parada de Servicios en Vivo y Huellas de Proceso

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Prácticas ingenuas y comandos peligrosos que resultan en baneo inmediato durante la ejecución de revisiones manuales en CMD o PowerShell.

---

## 1. Detener Servicios con `sc stop` o `Stop-Service`
- **Técnica errónea**:
  ```cmd
  sc stop "PcaSvc"
  sc stop "DPS"
  sc stop "SysMain"
  ```
- **Por qué falla ante los comandos del revisor (Baneo Inmediato)**:
  - Cuando el revisor de screenshare corre su rutina habitual:
    ```powershell
    get-service | findstr -i "pcasvc"; get-service | findstr -i "DPS"
    ```
    La consola muestra `Stopped` o no produce salida.
  - El revisor no necesita abrir herramientas avanzadas: este resultado en texto plano en la propia terminal de PowerShell es causal directa de **Ban Permanente por Servicios Manipulados**.

---

## 2. Mantener Procesos de Cheat Activos en Segundo Plano
- **Técnica errónea**: Dejar ejecutándose un inyector, una consola de bypass o un spoofer minimizado mientras se juega.
- **Por qué falla**:
  - Al ejecutar:
    ```powershell
    Get-WmiObject Win32_Process | Select-Object Name, ProcessId, CommandLine
    ```
    El script enumera todos los procesos y sus argumentos exactos de lanzamiento.
  - Rutas como `C:\Users\...\AppData\Local\Temp\loader.exe` con argumentos sospechosos (`-inject`, `-target javaw.exe`) quedan completamente expuestas.

---

## 3. Descargar y Ejecutar el Loader en Disco Directo de `C:\` sin Timestomping
- **Técnica errónea**: Descargar el archivo `.exe` al Escritorio o Descargas y ejecutarlo minutos antes de entrar a jugar.
- **Por qué falla**:
  - Al ejecutar el comando:
    ```cmd
    forfiles /P C:\ /S /D -1 /C "cmd /c if @isdir==FALSE echo @path @fdate @ftime"
    ```
    El comando barre todo el disco y escupe la ruta completa del archivo con la fecha de hoy y la hora exacta de hace unos minutos.
  - Incluso si el archivo fue borrado con `DeleteFile`, si no se purga la MFT o se usa un contenedor VHD, el registro de `$MFT` o el residuo en disco lo delata.

---

## 4. Provocar Dirty Bit en el Volumen NTFS
- **Técnica errónea**: Forzar apagados bruscos mediante pantallazos azules deliberados o desmontajes forzados de volumen para evitar que el revisor revise el equipo.
- **Por qué falla**:
  - Al reiniciar, el sistema queda marcado como sucio.
  - Al ejecutar:
    ```cmd
    fsutil dirty query C:
    ```
    La salida reporta **`Volume - C: is Dirty`**, lo que levanta una sospecha grave de sabotaje del sistema para eludir la revisión.
