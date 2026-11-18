@echo off
:: build.bat -- configura CMake y compila BypassWrath (Release x64).
:: Requiere: Visual Studio 2022 (cualquier edicion), CMake en PATH.
setlocal

set ROOT=%~dp0
set BUILD=%ROOT%build
set BIN=%ROOT%bin

echo [*] Configurando CMake...
cmake -B "%BUILD%" -S "%ROOT%" -G "Visual Studio 18 2026" -A x64 -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo [!] cmake configure fallo.
    exit /b 1
)

echo [*] Compilando...
cmake --build "%BUILD%" --config Release --parallel
if errorlevel 1 (
    echo [!] cmake build fallo.
    exit /b 1
)

echo.
echo [+] Build completo. Archivos en: %BIN%
dir "%BIN%\*.exe" "%BIN%\*.dll" 2>nul
endlocal
