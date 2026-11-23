# Bypass Limpio: Integridad Total de Hilos y Permisos del Sistema

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass rechaza categóricamente la suspensión de hilos y la alteración de permisos en el registro. La arquitectura de Wrath garantiza que todos los servicios y sus hilos operen en su estado natural sin sabotaje de telemetría.

---

## 1. Vector Principal: Cero Suspensión de Hilos en Servicios
*Regla de diseño de Wrath / VANTA:*

1. **Doctrina de No Interferencia de Hilos**:
   - Wrath jamás abre handles a procesos `svchost.exe` para llamar a `SuspendThread` ni `NtSuspendThread`.
   - No se interceptan hilos de `dps.dll`, `pcasvc.dll`, `sechost.dll` ni `wevtsvc.dll`.
2. **Resultado en la Inspección de System Informer**:
   - Al abrir la pestaña *Threads* de cualquier proceso `svchost.exe` hospeda de servicios:
     - El 100% de los hilos se encuentran en sus estados nativos (`Waiting` o `Running`).
     - Cero hilos en estado `Suspended`.
3. **Superación de la Prueba Dinámica en Vivo**:
   - Si el revisor lanza `calc.exe` o `notepad.exe` para comprobar el servicio, Windows genera de inmediato el archivo prefetch correspondiente y su registro en BAM con total normalidad.
   - No existe asincronía ni vacíos temporales.

---

## 2. Preservación Estricta de la Herencia de Permisos en el Registro
*Regla de seguridad de Wrath / VANTA:*

1. **Cero Manipulación de DACLs**:
   - Wrath jamás altera los permisos de seguridad de las claves del registro.
   - Las claves `BAM`, `RecentDocs`, `UserAssist`, `RunMRU` conservan:
     - Su herencia activa (`SE_DACL_AUTO_INHERITED`).
     - Control total (`KEY_ALL_ACCESS`) para la cuenta `NT AUTHORITY\SYSTEM` y el grupo `Administrators`.
2. **Resultado ante Escáneres Automáticos**:
   - Echo, Paladin y Ocean auditan los descriptores de seguridad del registro sin encontrar permisos denegados ni herencias rotas.

---

## 3. Teardown Sincronizado vs Bloqueo Preventivo
- La filosofía de Wrath se basa en no bloquear la creación de evidencias en caliente (lo que siempre deja firmas de sabotaje), sino en:
  1. Aislar la ejecución dentro del contenedor VHD montado sin letra de unidad (`NON_PNP`).
  2. Al finalizar la sesión, ejecutar la rutina nativa `wrath::trace::teardown` (`teardown.cpp`), la cual restaura los timestamps y limpia el slack space sin alterar el funcionamiento continuo de los servicios de Windows.
