@echo off
rem Removes build output and object files. Icons in bin\*.ico are kept
rem (they are regenerated only when missing).
rem User data (bin\layout.txt, bin\layout-*.txt, bin\admin.log, bin\Config.ini) is NOT touched:
rem clean.bat must never destroy a saved layout.
setlocal enableextensions
cd /d "%~dp0"

if exist "bin\ScreenMemory.exe" (
    del /q "bin\ScreenMemory.exe"
    echo [clean] removed bin\ScreenMemory.exe
)
if exist "T:\ScreenMemory\obj" (
    del /q "T:\ScreenMemory\obj\*.*" >nul 2>&1
    echo [clean] cleared T:\ScreenMemory\obj
)

echo [clean] done
exit /b 0
