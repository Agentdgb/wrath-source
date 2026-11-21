<#
.SYNOPSIS
    verify.ps1 -- post-test verification for R1 / R2.
.DESCRIPTION
    Checks the eleven R1 services, scans 4688 events for the forbidden
    lolbins and for "--scrub" on any command line, and lists the manual
    checks that cannot be automated from PowerShell.
#>
[CmdletBinding()]
param([int]$WindowMinutes = 30)

$ErrorActionPreference = "Continue"

$required = @(
    "DPS","DiagTrack","AppInfo","Dusmsvc","WSearch","BAM",
    "SysMain","PcaSvc","EventLog","Schedule","DcomLaunch"
)
$lolbins = @("msbuild","mshta","rundll32","regsvr32","wmic","bitsadmin","curl","certutil")
$fail = 0

Write-Host "=== R1: servicios ===" -ForegroundColor Cyan
foreach ($name in $required) {
    $svc = Get-Service -Name $name -ErrorAction SilentlyContinue
    if (-not $svc) {
        Write-Host "  [FAIL] $name : no existe" -ForegroundColor Red
        $fail++
    } elseif ($svc.Status -ne "Running") {
        Write-Host "  [FAIL] $name : $($svc.Status)" -ForegroundColor Red
        $fail++
    } else {
        Write-Host "  [ OK ] $name : Running" -ForegroundColor Green
    }
}

Write-Host "`n=== R2: eventos 4688 ===" -ForegroundColor Cyan
$since = (Get-Date).AddMinutes(-$WindowMinutes)
$procs = @()
try {
    $procs = Get-WinEvent -FilterHashtable @{
        LogName = "Security"; Id = 4688; StartTime = $since
    } -ErrorAction Stop
} catch {
    Write-Host "  [SKIP] no se pudieron leer eventos 4688 (requiere Admin / auditura activa)" -ForegroundColor Yellow
}

foreach ($e in $procs) {
    $xml = [xml]$e.ToXml()
    $cmd = $xml.Event.EventData.Data |
        Where-Object { $_.Name -eq "CommandLine" } |
        Select-Object -First 1 -ExpandProperty "#text"
    $img = $xml.Event.EventData.Data |
        Where-Object { $_.Name -eq "NewProcessName" } |
        Select-Object -First 1 -ExpandProperty "#text"

    if ($cmd -and $cmd -match "--scrub") {
        Write-Host "  [FAIL] --scrub en 4688: $cmd" -ForegroundColor Red
        $fail++
    }
    foreach ($lb in $lolbins) {
        if (($img -and $img -match $lb) -or ($cmd -and $cmd -match $lb)) {
            Write-Host "  [FAIL] lolbin $lb : $img $cmd" -ForegroundColor Red
            $fail++
        }
    }
}
if ($procs.Count -gt 0 -and $fail -eq 0) {
    Write-Host "  [ OK ] sin --scrub ni lolbins en $($procs.Count) eventos" -ForegroundColor Green
}

Write-Host "`n=== Chequeo manual pendiente ===" -ForegroundColor Cyan
@(
    "MEM_PRIVATE confirmado en el modulo mapeado de javaw.exe",
    "strings a cero (bypass_payload / PayloadInit / JNI_OnLoad / bp:: / .?A)",
    "sin RuntimeBroker-*.pf tras la salida del child",
    "PendingFileRenameOperations sin entradas del loader",
    "clave TMP_BWAMC inexistente",
    "valores Pca borrados y LastWriteTime restaurado",
    "ejecucion desde %TEMP%"
) | ForEach-Object { Write-Host "  [ ] $_" -ForegroundColor Gray }

Write-Host "`nRESULTADO: $fail fallo(s) automatico(s)." -ForegroundColor $(if ($fail) { "Red" } else { "Green" })
exit $fail
