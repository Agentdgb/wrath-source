param(
    [int]$TargetPid = 0,
    [switch]$SelfTest,
    [string]$DiagnosticsDirectory
)
$ErrorActionPreference = 'Stop'

function Get-RangeState([UInt64]$Start, [UInt64]$Length, [scriptblock]$Query) {
    if ($Start -eq 0 -or $Length -eq 0 -or $Length -gt 1GB -or $Start -gt [UInt64]::MaxValue - $Length) {
        return 'invalid_baseline'
    }
    $end = $Start + $Length
    $cursor = $Start
    $allocated = $false
    for ($step = 0; $step -lt 65536 -and $cursor -lt $end; $step++) {
        $region = & $Query $cursor
        if ($null -eq $region) { return 'query_failed' }
        $next = [UInt64]$region.Base + [UInt64]$region.Size
        if ($region.Base -gt $cursor -or $region.Size -eq 0 -or $next -le $cursor) { return 'query_failed' }
        if ($region.State -ne 0x10000) { $allocated = $true } # MEM_FREE
        $cursor = $next
    }
    if ($cursor -lt $end) { return 'query_failed' }
    if ($allocated) { return 'allocated_or_reused' }
    return 'original_region_free'
}

if ($SelfTest) {
    $free = { param($address) [pscustomobject]@{Base=0;Size=65536;State=0x10000} }
    $used = { param($address) [pscustomobject]@{Base=$address;Size=4096;State=0x1000} }
    $partial = { param($address) [pscustomobject]@{Base=$address;Size=4096;State=$(if($address -eq 4096){0x1000}else{0x10000})} }
    $failed = { param($address) $null }
    if ((Get-RangeState 4096 4096 $free) -ne 'original_region_free') { throw 'Free range rejected' }
    if ((Get-RangeState 4096 4096 $used) -ne 'allocated_or_reused') { throw 'Allocated range accepted as free' }
    if ((Get-RangeState 4096 8192 $partial) -ne 'allocated_or_reused') { throw 'Partially allocated range accepted as free' }
    if ((Get-RangeState 4096 4096 $failed) -ne 'query_failed') { throw 'Failed query accepted' }
    if ((Get-RangeState 4096 0 $free) -ne 'invalid_baseline') { throw 'Invalid range accepted' }
    Write-Output 'PASS: free, allocated, partial, failed-query and invalid-baseline cases.'
    exit 0
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WrathUnloadQuery {
    [StructLayout(LayoutKind.Sequential)]
    public struct MemoryInfo {
        public IntPtr BaseAddress;
        public IntPtr AllocationBase;
        public uint AllocationProtect;
        public UIntPtr RegionSize;
        public uint State;
        public uint Protect;
        public uint Type;
    }
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern UIntPtr VirtualQueryEx(IntPtr process, IntPtr address, out MemoryInfo info, UIntPtr length);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr handle);
}
'@

if ($TargetPid -eq 0) {
    $candidates = @(Get-Process -Name javaw -ErrorAction SilentlyContinue)
    if ($candidates.Count -ne 1) { throw 'Indica el PID de Minecraft: -TargetPid 1234. Debe haber un solo javaw para seleccionarlo automaticamente.' }
    $TargetPid = $candidates[0].Id
}
$directory = if ($DiagnosticsDirectory) {
    [IO.Path]::GetFullPath($DiagnosticsDirectory)
} else { Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'WrathClient\diagnostics' }
$result = [ordered]@{
    pid=$TargetPid; checked_at=[DateTime]::UtcNow.ToString('o'); status='inconclusive';
    last_reported_method=$null; method_time_near_baseline=$false; matching_registered_modules=$null;
    module_enumeration_error=$null; region_status='no_baseline'; unload_requested=$false;
    preserve_data=$true; limitation='La region original libre no demuestra ausencia de otras copias ni de historial del sistema.'
}
$process = Get-Process -Id $TargetPid -ErrorAction SilentlyContinue
if ($null -eq $process) {
    $result.status = 'process_exited'
} else {
    $identity = $process.StartTime.ToUniversalTime().ToFileTimeUtc()
    try {
        $result.matching_registered_modules = @($process.Modules | Where-Object {
            $_.ModuleName -match '(?i)wrath|vanta|^d3dcache\.dll$'
        } | ForEach-Object {$_.ModuleName})
    } catch { $result.module_enumeration_error = $_.Exception.Message }
    $baselinePath = Join-Path $directory "module-$TargetPid.json"
    if (Test-Path -LiteralPath $baselinePath) {
        $baseline = Get-Content -LiteralPath $baselinePath -Raw | ConvertFrom-Json
        if ($baseline.version -ne 1 -or $baseline.pid -ne $TargetPid -or $baseline.process_start -ne $identity) {
            $result.region_status = 'stale_or_invalid_baseline'
        } else {
            $loadPath = Join-Path $directory "load-$TargetPid.json"
            if (Test-Path -LiteralPath $loadPath) {
                $load = Get-Content -LiteralPath $loadPath -Raw | ConvertFrom-Json
                if ($load.version -eq 1 -and $load.pid -eq $TargetPid -and $load.process_start -eq $identity) {
                    $result.last_reported_method = $load.method
                    # The loader records a completed operation near bootstrap.
                    # An old record from another cycle must not be called certain.
                    $difference = [Math]::Abs([double]$load.recorded_at - [double]$baseline.recorded_at)
                    $result.method_time_near_baseline = $difference -le 300000000 # Temporal clue only, not a session identifier.
                }
            }
            $unloadPath = Join-Path $directory "unload-$TargetPid.json"
            if (Test-Path -LiteralPath $unloadPath) {
                $unload = Get-Content -LiteralPath $unloadPath -Raw | ConvertFrom-Json
                $result.unload_requested = $unload.version -eq 1 -and $unload.pid -eq $TargetPid -and
                    $unload.process_start -eq $identity -and $unload.recorded_at -ge $baseline.recorded_at
            }
            # Query page metadata only. No memory contents, writes or remote code.
            $handle = [WrathUnloadQuery]::OpenProcess(0x400, $false, $TargetPid)
            if ($handle -eq [IntPtr]::Zero) { $result.region_status = 'access_denied' }
            else {
                try {
                    $query = {
                        param($address)
                        $memory = New-Object WrathUnloadQuery+MemoryInfo
                        $size = [Runtime.InteropServices.Marshal]::SizeOf($memory)
                        $read = [WrathUnloadQuery]::VirtualQueryEx($handle, [IntPtr][long]$address,
                            [ref]$memory, [UIntPtr][uint64]$size)
                        if ($read -eq [UIntPtr]::Zero) { return $null }
                        [pscustomobject]@{Base=[UInt64]$memory.BaseAddress.ToInt64();Size=$memory.RegionSize.ToUInt64();State=$memory.State}
                    }
                    $result.region_status = Get-RangeState ([UInt64]$baseline.allocation_base) ([UInt64]$baseline.allocation_size) $query
                    if ($result.region_status -eq 'original_region_free') { $result.status = 'original_region_confirmed_free' }
                } finally { [void][WrathUnloadQuery]::CloseHandle($handle) }
            }
        }
    }
}
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$reportPath = Join-Path $directory ("check-$TargetPid-" + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff') + '.json')
$json = $result | ConvertTo-Json -Depth 5
$json | Set-Content -LiteralPath $reportPath -Encoding UTF8
Write-Output $json
Write-Output "Informe: $reportPath"
