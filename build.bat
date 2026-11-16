@echo off
rem ============================================================================
rem  ScreenMemory - build bin\ScreenMemory.exe
rem  vcvars64 -> rc.exe -> cl.exe. Objects/PDB go to RAM disk T: (less SSD wear).
rem  Messages are ASCII on purpose: cmd.exe cannot reliably echo UTF-8 batch text.
rem ============================================================================
setlocal enableextensions
cd /d "%~dp0"

rem --- 1. Locate MSVC environment ---------------------------------------------
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (
    echo [build] ERROR: vcvars64.bat not found. Install Visual Studio with C++ x64 tools.
    exit /b 1
)

rem --- 2. Environment ----------------------------------------------------------
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [build] ERROR: failed to initialize MSVC environment.
    exit /b 1
)

rem --- 3. Directories ----------------------------------------------------------
set "OBJ=T:\ScreenMemory\obj"
if not exist "%OBJ%" mkdir "%OBJ%" >nul 2>&1
if exist "%OBJ%" goto :obj_ready
rem RAM disk T: is missing (not mounted / drive letter changed): fall back to %TEMP%
rem so the build still works. Objects are disposable, this path costs nothing.
set "OBJ=%TEMP%\ScreenMemory\obj"
if not exist "%OBJ%" mkdir "%OBJ%" >nul 2>&1
if not exist "%OBJ%" (
    echo [build] ERROR: cannot create object directory "%OBJ%".
    exit /b 1
)
echo [build] WARNING: RAM disk T: is not available, objects go to "%OBJ%"
:obj_ready
echo [build] objects: %OBJ%
if not exist "bin" mkdir "bin"

rem --- 4. Icons (generated once) -----------------------------------------------
if not exist "bin\save.ico"    call :icons || exit /b 1
if not exist "bin\restore.ico" call :icons || exit /b 1

rem --- 5. Resources -------------------------------------------------------------
echo [build] rc.exe: ScreenMemory.rc
rc /nologo /fo "%OBJ%\app.res" ScreenMemory.rc
if errorlevel 1 (
    echo [build] ERROR: rc.exe failed.
    exit /b 1
)

rem --- 6. Compile and link -----------------------------------------------------
echo [build] cl.exe: src\*.cpp
cl /nologo /W4 /std:c++17 /EHsc /O2 /MT /permissive- /utf-8 ^
   /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 ^
   /Fo:%OBJ%\ /Fd:%OBJ%\vc.pdb ^
   /Fe:bin\ScreenMemory.exe ^
   src\*.cpp "%OBJ%\app.res" ^
   /link /SUBSYSTEM:CONSOLE /MANIFEST:EMBED /MANIFESTINPUT:ScreenMemory.manifest ^
   user32.lib dwmapi.lib ole32.lib oleaut32.lib shell32.lib shlwapi.lib ^
   psapi.lib propsys.lib advapi32.lib
if errorlevel 1 (
    echo [build] ERROR: compilation failed.
    exit /b 1
)

echo.
echo [build] done: bin\ScreenMemory.exe
exit /b 0

:icons
echo [build] generating icons bin\save.ico, bin\restore.ico
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\make_icons.ps1"
if errorlevel 1 (
    echo [build] ERROR: icon generation failed.
    exit /b 1
)
exit /b 0
