# gen_salt.ps1 - Generates build_salt.h with a cryptographically fresh 64-bit salt
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$stringsDir = Join-Path (Split-Path -Parent $scriptDir) "strings"
if (-not (Test-Path $stringsDir)) {
    New-Item -ItemType Directory -Path $stringsDir -Force | Out-Null
}

$saltBytes = New-Object byte[] 8
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
$rng.GetBytes($saltBytes)
$saltHex = -join ($saltBytes | ForEach-Object { '{0:X2}' -f $_ })

$targetFile = Join-Path $stringsDir "build_salt.h"
$content = @"
#pragma once
#ifndef WRATH_BUILD_SALT_H
#define WRATH_BUILD_SALT_H

#include <cstdint>

#define BUILD_SALT 0x${saltHex}ULL

#endif // WRATH_BUILD_SALT_H
"@

Set-Content -Path $targetFile -Value $content -Encoding utf8
Write-Host "Generated build_salt.h at $targetFile with salt 0x$saltHex"
