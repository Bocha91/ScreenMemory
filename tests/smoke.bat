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
rem  New in 0.9.1: restore --from (an existing file, a missing file, a missing
rem  value, a wrong command) and the day chain - a snapshot taken before the
rem  current boot must be copied into the "yesterday" slot exactly once, and a
rem  repeated restore must not add another copy. The chain check builds a fake
rem  0-window snapshot, so that real restore moves nothing.
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
call :run "restore --dry-run --from layout.txt" 0
call :run "restore --dry-run --from missing-layout.txt" 2
call :run "restore --from" 64
call :run "save --from layout.txt" 64
call :run "AutoSave --dry-run" 0
call :run "AutoRestore --dry-run" 0
call :run "Uninstall --dry-run" 0
call :run "bogus" 64
call :run "--list save" 64
call :run "save restore" 64
call :run "AutoSave restore" 64
call :run "AutoSave --list" 64
call :run "--list --version" 64
call :checkchain "%SANDBOX%"
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

rem Day chain (0.9.1): a snapshot older than the current boot is copied into the
rem "yesterday" slot by restore, and only once - the next restore sees the copy
rem already in place and does not shift the chain again. The fake snapshot has no
rem windows and no monitors, so the restores below move nothing; layout.txt is
rem replaced and the sandbox is throwaway anyway.
:checkchain
set "D=%~1"
> "%D%\layout.txt" (
    echo # ScreenMemory layout v1
    echo # saved=2020-01-01T00:00:00  screen=1920x1080  monitors=0  desktops=0  windows=0
)
"%D%\ScreenMemory.exe" restore >nul 2>&1
set "GOT=%errorlevel%"
if not "%GOT%"=="0" (
    echo [smoke] FAIL  restore of an old snapshot -^> %GOT%, expected 0
    set /a FAILED+=1
    exit /b 0
)
call :protected "%D%" CH1
"%D%\ScreenMemory.exe" restore >nul 2>&1
call :protected "%D%" CH2
if "%CH1%"=="1" if "%CH2%"=="1" (
    echo [smoke] ok    day chain: 1 protected copy, second restore did not shift it again
) else (
    echo [smoke] FAIL  day chain: %CH1% protected copies after restore, %CH2% after the second, expected 1
    set /a FAILED+=1
)
exit /b 0

rem Protected copies are the ones whose name is not layout-<date>: the day chain
rem and the "previous configuration" chain. Count = all layout-*.txt minus the
rem timestamped working versions (those start with a date year).
:protected
set "ALL=0"
for /f %%c in ('dir /b /a-d "%~1\layout-*.txt" 2^>nul ^| find /c /v ""') do set "ALL=%%c"
set "TS=0"
for /f %%c in ('dir /b /a-d "%~1\layout-2*.txt" 2^>nul ^| find /c /v ""') do set "TS=%%c"
set /a "%~2=ALL-TS"
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
