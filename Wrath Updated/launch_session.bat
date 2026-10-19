@echo off
:: ============================================================
::  launch_session.bat  –  Wrath Session Launcher (On-Demand)
:: ============================================================
setlocal EnableDelayedExpansion

title Wrath Client - Launcher

:: ── 0. Verificar privilegios de Administrador ────────────────
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [!] Este lanzador requiere permisos de Administrador.
    echo     Haz clic derecho y selecciona "Ejecutar como administrador".
    pause & exit /b 1
)

:: ── 1. Esperar / Verificar que Minecraft este abierto ────────
echo ============================================================
echo            WRATH CLIENT - MODO LOCAL ON-DEMAND
echo ============================================================
echo.
echo [*] Verificando estado de Minecraft (javaw.exe)...

tasklist /fi "imagename eq javaw.exe" 2>nul | find /i "javaw.exe" >nul
if %errorlevel% neq 0 (
    echo [i] Minecraft no esta abierto.
    echo [*] Abre Minecraft 1.21.11 (Fabric) ahora...
    echo [*] Esperando deteccion automatica (o pulsa Ctrl+C para cancelar)...
    
    :WAIT_MINECRAFT
    timeout /t 2 /nobreak >nul
    tasklist /fi "imagename eq javaw.exe" 2>nul | find /i "javaw.exe" >nul
    if %errorlevel% neq 0 goto WAIT_MINECRAFT
)

echo [+] Minecraft detectado exitosamente en ejecucion.
echo.

:: ── 2. Iniciar el servidor de autenticacion local temporal ───
echo [*] Iniciando servidor de autenticacion y bot de Discord temporalmente...
start /b "" python "%~dp0Server\server.py" >nul 2>&1

:: Dar 2 segundos para sincronizar con Discord
timeout /t 2 /nobreak >nul

:: ── 3. Ejecutar WrathLoader ──────────────────────────────────
set "LOADER=%~dp0WrathLoader.exe"
if not exist "%LOADER%" (
    echo [!] No se encontro WrathLoader.exe en %LOADER%
    goto CLEANUP
)

echo [*] Abriendo WrathLoader...
start /wait "" "%LOADER%" %*

:: ── 4. Limpieza automatica: Detener servidor al terminar ────
:CLEANUP
echo.
echo [*] WrathLoader ha finalizado la inyeccion o se ha cerrado.
echo [*] Cerrando servidor local de autenticacion...

for /f "tokens=5" %%a in ('netstat -aon ^| findstr :8000 ^| findstr LISTENING') do taskkill /f /pid %%a >nul 2>&1
taskkill /f /fi "imagename eq python.exe" /fi "windowtitle eq Wrath*" >nul 2>&1

echo [+] Servidor cerrado. Cero procesos en segundo plano.
echo [+] Sesion completada.
timeout /t 3 /nobreak >nul
endlocal
exit /b 0
