@echo off
rem ============================================================================
rem  Smoke test: builds nothing, only checks CLI contract of bin\ScreenMemory.exe
rem  (help/version/no-args/save/--list/--dry-run/autostart/unknown argument +
rem  exit codes). Expected codes: 0 ok, 1 restore applied partially, 2 no
rem  layout.txt, 3 no desktop list, 5 administrator rights required, 64 usage
rem  error. restore --dry-run is used instead of a real restore: the real one
rem  moves live windows around the desktop. AutoSave/AutoRestore/Uninstall are
rem  checked with --dry-run only: a real call would register (or delete) real
rem  Scheduled Tasks and needs elevation, which a smoke test must not depend on.
rem  save is checked twice: exit code 0 and the snapshot file it must create.
rem
rem  The exe is copied into a throwaway sandbox and runs from there: the program
rem  keeps layout.txt / admin.log next to itself, and "save" would otherwise
rem  overwrite the real snapshot in bin\ (this is how the 5-desktop baseline of
rem  the TestFallback experiment was lost on 13.11.2026).
rem ============================================================================
setlocal enableextensions
cd /d "%~dp0.."

if not exist "bin\ScreenMemory.exe" (
    echo [smoke] ERROR: bin\ScreenMemory.exe not found. Run build.bat first.
    exit /b 1
)

set "SANDBOX=%TEMP%\screenmemory-smoke"
if exist "%SANDBOX%" rd /s /q "%SANDBOX%"
mkdir "%SANDBOX%" 2>nul
copy /y "bin\ScreenMemory.exe" "%SANDBOX%\ScreenMemory.exe" >nul
if not exist "%SANDBOX%\ScreenMemory.exe" (
    echo [smoke] ERROR: cannot copy exe to "%SANDBOX%".
    exit /b 1
)
set "EXE=%SANDBOX%\ScreenMemory.exe"
del /q "%TEMP%\ScreenMemory-AutoSave.xml" >nul 2>&1
del /q "%TEMP%\ScreenMemory-AutoRestore.xml" >nul 2>&1

set "FAILED=0"
call :run "--help" 0
call :run "--version" 0
call :run "" 0
call :run "save" 0
call :checkfile "%SANDBOX%\layout.txt"
call :checknorotate "%SANDBOX%"
call :run "--list" 0
call :run "restore --dry-run" 0
call :run "restore --dry-run --verbose" 0
call :run "AutoSave --dry-run" 0
call :run "AutoRestore --dry-run" 0
call :run "Uninstall --dry-run" 0
call :run "bogus" 64
call :run "--list save" 64
call :run "save restore" 64
call :run "AutoSave restore" 64
call :run "AutoSave --list" 64
call :run "--list --version" 64
del /q "%TEMP%\ScreenMemory-AutoSave.xml" >nul 2>&1
del /q "%TEMP%\ScreenMemory-AutoRestore.xml" >nul 2>&1

echo [smoke] failures: %FAILED%
if "%FAILED%"=="0" (echo [smoke] PASS) else (echo [smoke] FAIL)
exit /b %FAILED%

:checkfile
if exist "%~1" (
    echo [smoke] ok    created "%~1"
) else (
    echo [smoke] FAIL  missing "%~1"
    set /a FAILED+=1
)
exit /b 0

rem Second save on an unchanged layout must not write the file and must not
rem rotate the previous snapshot. The saved stamp (# saved=<ISO time>) is
rem inside layout.txt, so a rewrite always changes the file bytes; comparing a
rem copy also survives time stamp resolution of only one minute. A live desktop
rem can change between two runs (a window opens, a browser finishes loading),
rem which legitimately rewrites the file, so the check is retried up to three
rem times: a real regression rewrites the file every time.
:checknorotate
set "TRIES=0"
:nr_try
set /a TRIES+=1
call :onesave "%~1"
if "%SAME%"=="1" goto nr_ok
if %TRIES% LSS 3 goto nr_try
echo [smoke] FAIL  save rewrote layout.txt although nothing changed, attempts %TRIES%
set /a FAILED+=1
exit /b 0
:nr_ok
echo [smoke] ok    unchanged layout -^> file not rewritten
exit /b 0

:onesave
set "ROT=0"
for /f %%c in ('dir /b /a-d "%~1\layout-*.txt" 2^>nul ^| find /c /v ""') do set "ROT=%%c"
copy /y "%~1\layout.txt" "%~1\layout.before" >nul
"%~1\ScreenMemory.exe" save >nul 2>&1
set "RC=%errorlevel%"
set "ROT2=0"
for /f %%c in ('dir /b /a-d "%~1\layout-*.txt" 2^>nul ^| find /c /v ""') do set "ROT2=%%c"
set "SAME=0"
if not "%RC%"=="0" goto os_clean
if not "%ROT%"=="%ROT2%" goto os_clean
fc /b "%~1\layout.before" "%~1\layout.txt" >nul 2>&1
if "%errorlevel%"=="0" set "SAME=1"
:os_clean
del /q "%~1\layout.before" >nul 2>&1
exit /b 0

:run
set "ARGS=%~1"
set "WANT=%~2"
if "%ARGS%"=="" (
    "%EXE%" >nul 2>&1
) else (
    "%EXE%" %ARGS% >nul 2>&1
)
set "GOT=%errorlevel%"
if "%GOT%"=="%WANT%" (
    echo [smoke] ok    "%ARGS%" -^> %GOT%
) else (
    echo [smoke] FAIL  "%ARGS%" -^> %GOT%, expected %WANT%
    set /a FAILED+=1
)
exit /b 0
