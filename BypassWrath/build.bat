@echo off
:: build.bat -- configura CMake y compila BypassWrath (Release x64, VS18).
setlocal

set ROOT=%~dp0
set BUILD=%ROOT%build
set BIN=%ROOT%bin\Release

if exist "%BUILD%\CMakeCache.txt" (
    echo [*] Cache existente, usando build incremental.
) else (
    echo [*] Configurando CMake...
    cmake -B "%BUILD%" -S "%ROOT%" -G "Visual Studio 18 2026" -A x64
    if errorlevel 1 ( echo [!] cmake configure fallo. & exit /b 1 )
)

echo [*] Compilando...
cmake --build "%BUILD%" --config Release --parallel
if errorlevel 1 ( echo [!] cmake build fallo. & exit /b 1 )

echo.
echo [+] Build completo:
dir "%BIN%\*.exe" "%BIN%\*.dll" 2>nul
endlocal
