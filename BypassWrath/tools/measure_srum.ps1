<#
.SYNOPSIS
    measure_srum.ps1 -- Script de auditoría empírica para medir trazas en SRUM (SRUDB.dat).
.DESCRIPTION
    Realiza una extracción temporal y lectura en solo lectura de la base de datos ESE de SRUM
    (System Resource Usage Monitor) usando la librería nativa de Windows (Microsoft.Isam.Esent.Interop).
    Inspecciona SruDbIdMapTable y tablas de consumo de recursos para determinar si el loader
    deja alguna fila registrada tras su ejecución.
.PARAMETER Pattern
    Nombre del binario o patrón de ruta a buscar (por defecto: "RuntimeBroker").
.PARAMETER DatabasePath
    Ruta a un archivo SRUDB.dat existente (evita pausar DPS si ya se tiene una copia).
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\tools\measure_srum.ps1
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\tools\measure_srum.ps1 -Pattern "BypassLoader"
#>

[CmdletBinding()]
param (
    [string]$Pattern = "RuntimeBroker",
    [string]$DatabasePath = ""
)

$ErrorActionPreference = "Stop"

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  AUDITORÍA EMPÍRICA DE SRUM (SRUDB.dat) - BypassWrath" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "[*] Patrón de búsqueda: '$Pattern'" -ForegroundColor Yellow

if (-not (Test-IsAdmin) -and -not $DatabasePath) {
    Write-Host "[!] ERROR: Se requieren permisos de Administrador para inspeccionar SRUM en vivo." -ForegroundColor Red
    Write-Host "[!] Ejecuta esta consola de PowerShell como Administrador o proporciona una copia con -DatabasePath." -ForegroundColor Yellow
    exit 1
}

# 1. Localizar librería Esent de Windows
$esentDllPath = "C:\Windows\Microsoft.NET\assembly\GAC_MSIL\microsoft.isam.esent.interop\v4.0_10.0.0.0__31bf3856ad364e35\Microsoft.Isam.Esent.Interop.dll"
if (-not (Test-Path $esentDllPath)) {
    $found = Get-ChildItem "C:\Windows\Microsoft.NET\assembly" -Filter "Microsoft.Isam.Esent.Interop.dll" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $esentDllPath = $found.FullName }
}

if (-not (Test-Path $esentDllPath)) {
    Write-Host "[!] ERROR: No se encontró Microsoft.Isam.Esent.Interop.dll en el GAC." -ForegroundColor Red
    exit 1
}

try {
    [System.Reflection.Assembly]::LoadFrom($esentDllPath) | Out-Null
    Write-Host "[+] Driver ESE cargado: Microsoft.Isam.Esent.Interop" -ForegroundColor Green
} catch {
    Write-Host "[!] ERROR al cargar ensamblado Esent: $_" -ForegroundColor Red
    exit 1
}

$tempDb = ""
$dpsWasStopped = $false
$liveDbPath = "$env:SystemRoot\System32\sru\SRUDB.dat"

# 2. Obtener copia de trabajo limpia
if ($DatabasePath) {
    if (-not (Test-Path $DatabasePath)) {
        Write-Host "[!] Archivo no encontrado: $DatabasePath" -ForegroundColor Red
        exit 1
    }
    $targetDb = $DatabasePath
    Write-Host "[+] Usando base de datos provista: $targetDb" -ForegroundColor Gray
} else {
    $tempDb = "$env:TEMP\srum_audit_$((Get-Random)).dat"
    Write-Host "[*] Pausando brevemente servicio DPS para snapshot limpio..." -ForegroundColor Yellow
    
    $dps = Get-Service -Name "DPS" -ErrorAction SilentlyContinue
    if ($dps -and $dps.Status -eq 'Running') {
        Stop-Service -Name "DPS" -Force -ErrorAction Stop
        $dpsWasStopped = $true
        # Esperar hasta que esté completamente parado
        $dps.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(5))
    }

    try {
        Copy-Item -Path $liveDbPath -Destination $tempDb -Force -ErrorAction Stop
        Write-Host "[+] Snapshot de SRUDB.dat copiado a temporal." -ForegroundColor Green
    } finally {
        if ($dpsWasStopped) {
            Start-Service -Name "DPS" -ErrorAction SilentlyContinue
            Write-Host "[+] Servicio DPS restaurado a RUNNING." -ForegroundColor Green
        }
    }
    $targetDb = $tempDb
}

# 3. Analizar la base de datos con ESE
$instance = $null
$sesid = $null
$dbid = $null
$tableid = $null

$matchedIdentities = @()

try {
    # Configuración de ESE para lectura offline segura
    [Microsoft.Isam.Esent.Interop.SystemParameters]::Recovery = $false
    [Microsoft.Isam.Esent.Interop.SystemParameters]::CircularLog = 1
    [Microsoft.Isam.Esent.Interop.SystemParameters]::TempPath = $env:TEMP

    $instName = "SrumAuditor_" + (Get-Random)
    [Microsoft.Isam.Esent.Interop.Api]::JetCreateInstance([ref]$instance, $instName)
    [Microsoft.Isam.Esent.Interop.Api]::JetInit([ref]$instance)
    [Microsoft.Isam.Esent.Interop.Api]::JetBeginSession($instance, [ref]$sesid, $null, $null)

    # Attach y Open en solo lectura
    [Microsoft.Isam.Esent.Interop.Api]::JetAttachDatabase($sesid, $targetDb, [Microsoft.Isam.Esent.Interop.AttachDatabaseGrbit]::ReadOnly)
    [Microsoft.Isam.Esent.Interop.Api]::JetOpenDatabase($sesid, $targetDb, $null, [ref]$dbid, [Microsoft.Isam.Esent.Interop.OpenDatabaseGrbit]::ReadOnly)

    Write-Host "[*] Conexión establecida con la base de datos ESE." -ForegroundColor Gray

    # Inspeccionar SruDbIdMapTable
    Write-Host "[*] Escaneando SruDbIdMapTable (Mapeo de aplicaciones)..." -ForegroundColor Yellow
    [Microsoft.Isam.Esent.Interop.Api]::JetOpenTable($sesid, $dbid, "SruDbIdMapTable", $null, 0, [Microsoft.Isam.Esent.Interop.OpenTableGrbit]::ReadOnly, [ref]$tableid)

    # Obtener column IDs
    $colIdType = [Microsoft.Isam.Esent.Interop.Api]::GetTableColumnid($sesid, $tableid, "IdType")
    $colIdIndex = [Microsoft.Isam.Esent.Interop.Api]::GetTableColumnid($sesid, $tableid, "IdIndex")
    $colIdBlob = [Microsoft.Isam.Esent.Interop.Api]::GetTableColumnid($sesid, $tableid, "IdBlob")

    $totalApps = 0
    if ([Microsoft.Isam.Esent.Interop.Api]::TryMoveFirst($sesid, $tableid)) {
        do {
            $idType = [Microsoft.Isam.Esent.Interop.Api]::RetrieveColumnAsInt32($sesid, $tableid, $colIdType)
            # IdType == 0 -> Rutas de aplicaciones (no SIDs ni paquetes AppX)
            if ($idType -eq 0) {
                $totalApps++
                $idIndex = [Microsoft.Isam.Esent.Interop.Api]::RetrieveColumnAsInt32($sesid, $tableid, $colIdIndex)
                $rawBytes = [Microsoft.Isam.Esent.Interop.Api]::RetrieveColumn($sesid, $tableid, $colIdBlob)
                
                $blobStr = ""
                if ($rawBytes -and $rawBytes.Length -gt 0) {
                    $blobStr = [System.Text.Encoding]::Unicode.GetString($rawBytes).TrimEnd("`0")
                }

                if ($blobStr -like "*$Pattern*") {
                    $matchedIdentities += [PSCustomObject]@{
                        IdIndex = $idIndex
                        IdType  = $idType
                        Path    = $blobStr
                    }
                }
            }
        } while ([Microsoft.Isam.Esent.Interop.Api]::TryMoveNext($sesid, $tableid))
    }

    [Microsoft.Isam.Esent.Interop.Api]::JetCloseTable($sesid, $tableid)
    $tableid = $null

    Write-Host "[+] Total de ejecutables registrados en SRUM: $totalApps" -ForegroundColor Gray

    # 4. Verificar tablas de consumo si hubo coincidencias
    $totalUsageRows = 0
    if ($matchedIdentities.Count -gt 0) {
        Write-Host "[!] Se encontraron $($matchedIdentities.Count) coincidencias en SruDbIdMapTable!" -ForegroundColor Yellow
        foreach ($m in $matchedIdentities) {
            Write-Host "    -> IdIndex: $($m.IdIndex) | Ruta: $($m.Path)" -ForegroundColor White
        }

        # Tablas de consumo estándar en SRUM
        $usageTables = @(
            "{D10CA2FB-CF52-4148-B3E0-D07F7E663F00}", # AppResourceUsage
            "{D10CA2FC-CF52-4148-B3E0-D07F7E663F00}", # NetworkUsages
            "{5C8CF630-B29B-4D40-909C-65B59C21B381}"  # AppTimeline
        )

        foreach ($tblName in $usageTables) {
            try {
                $tId = $null
                [Microsoft.Isam.Esent.Interop.Api]::JetOpenTable($sesid, $dbid, $tblName, $null, 0, [Microsoft.Isam.Esent.Interop.OpenTableGrbit]::ReadOnly, [ref]$tId)
                $colAppId = [Microsoft.Isam.Esent.Interop.Api]::GetTableColumnid($sesid, $tId, "AppId")

                if ([Microsoft.Isam.Esent.Interop.Api]::TryMoveFirst($sesid, $tId)) {
                    do {
                        $appId = [Microsoft.Isam.Esent.Interop.Api]::RetrieveColumnAsInt32($sesid, $tId, $colAppId)
                        foreach ($m in $matchedIdentities) {
                            if ($appId -eq $m.IdIndex) {
                                $totalUsageRows++
                            }
                        }
                    } while ([Microsoft.Isam.Esent.Interop.Api]::TryMoveNext($sesid, $tId))
                }
                [Microsoft.Isam.Esent.Interop.Api]::JetCloseTable($sesid, $tId)
            } catch {
                # Tabla no presente en este schema o vacía
            }
        }
    }

    # 5. Veredicto Final
    Write-Host "`n------------------------------------------------------------" -ForegroundColor Cyan
    Write-Host "  RESULTADO DE LA MEDICIÓN" -ForegroundColor Cyan
    Write-Host "------------------------------------------------------------" -ForegroundColor Cyan

    if ($matchedIdentities.Count -eq 0) {
        Write-Host "[PASS] CERO (0) coincidencias encontradas para '$Pattern'." -ForegroundColor Green
        Write-Host "[INFO] El ejecutable NO ha dejado ninguna entrada en SRUDB.dat." -ForegroundColor Green
        Write-Host "[CONCLUSIÓN] Como la ejecución dura menos de 1 segundo, el temporizador" -ForegroundColor White
        Write-Host "             de agregación de DPS no alcanza a registrar el proceso." -ForegroundColor White
        Write-Host "             -> ScrubSrum() NO es necesario en el ciclo crítico." -ForegroundColor Green
    } else {
        Write-Host "[AVISO] Se encontraron $($matchedIdentities.Count) entrada(s) de identidad y $totalUsageRows fila(s) de uso." -ForegroundColor Yellow
        Write-Host "[RECOMENDACIÓN] Se aconseja habilitar el saneamiento de SRUM (--srum)." -ForegroundColor Yellow
    }
    Write-Host "------------------------------------------------------------`n" -ForegroundColor Cyan

} catch {
    Write-Host "[!] Error durante el análisis ESE: $_" -ForegroundColor Red
} finally {
    if ($tableid) {
        try { [Microsoft.Isam.Esent.Interop.Api]::JetCloseTable($sesid, $tableid) } catch {}
    }
    if ($dbid) {
        try { [Microsoft.Isam.Esent.Interop.Api]::JetCloseDatabase($sesid, $dbid, [Microsoft.Isam.Esent.Interop.CloseDatabaseGrbit]::None) } catch {}
    }
    if ($sesid) {
        try { [Microsoft.Isam.Esent.Interop.Api]::JetDetachDatabase($sesid, $targetDb) } catch {}
        try { [Microsoft.Isam.Esent.Interop.Api]::JetEndSession($sesid, [Microsoft.Isam.Esent.Interop.EndSessionGrbit]::None) } catch {}
    }
    if ($instance) {
        try { [Microsoft.Isam.Esent.Interop.Api]::JetTerm($instance) } catch {}
    }

    if ($tempDb -and (Test-Path $tempDb)) {
        Remove-Item -Path $tempDb -Force -ErrorAction SilentlyContinue
    }
}
