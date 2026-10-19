@echo off
:: ============================================================
::  launch_dev.bat – Wrath Client (Direct Developer Bypass)
:: ============================================================
title Wrath Client - Modo Developer (Bypass Total)

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [!] Este lanzador requiere permisos de Administrador.
    echo     Haz clic derecho y selecciona "Ejecutar como administrador".
    pause & exit /b 1
)

echo ============================================================
echo         WRATH CLIENT - DEV BYPASS (DIRECT INJECT)
echo ============================================================
echo [*] Iniciando WrathLoader en modo bypass directo sin servidor...
start "" "%~dp0WrathLoader.exe" --bypass
exit /b 0
