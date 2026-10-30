@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem collect-usbip-payload.bat
rem
rem Run INSIDE Windows Sandbox / a VM, AFTER the official USBip installer has been run once
rem (tick "Install Microsoft Visual C++ Redistributable" in the installer so the runtime DLLs get installed).
rem
rem Copies the minimal runtime payload (about 3 MB) into ONE folder that you then copy out of the Sandbox:
rem   <out>\bin\      devnode.exe usbip.exe libusbip.dll resources.dll
rem   <out>\driver\   usbip2_ude.inf_amd64_*  usbip2_filter.inf_amd64_*     (.inf .sys .cat - do not modify)
rem   <out>\runtime\  vcruntime140.dll vcruntime140_1.dll msvcp140.dll msvcp140_atomic_wait.dll
rem   <out>\MANIFEST.txt   size, SHA256 and file version of everything
rem
rem Usage:   collect-usbip-payload.bat [output-folder]        default: %USERPROFILE%\Desktop\usbip-payload
rem          The output folder must not exist yet (nothing is ever deleted by this script).
rem Test overrides (environment): USBIP_APPDIR  USBIP_STORE  USBIP_SYSDIR  NO_PAUSE  NO_EXPLORER
rem
rem NOTE: keep this file ASCII-only (no control characters either) with CRLF line endings: cmd.exe parses
rem batch files in the OEM code page. Inside parenthesised blocks, print paths with !VAR! (delayed expansion),
rem never %VAR%: a path such as "C:\Program Files (x86)\..." would close the block early.

set "OUT=%~1"
if not defined OUT set "OUT=%USERPROFILE%\Desktop\usbip-payload"
if not defined USBIP_APPDIR set "USBIP_APPDIR=%ProgramFiles%\USBip"
if not defined USBIP_STORE set "USBIP_STORE=%SystemRoot%\System32\DriverStore\FileRepository"
if not defined USBIP_SYSDIR set "USBIP_SYSDIR=%SystemRoot%\System32"

rem Double-clicked (not started from a console)? Then keep the window open at the end.
rem find.exe is called by full path: with Git for Windows on PATH, a bare "find" is GNU find.
set "DBLCLK="
echo %cmdcmdline% | "%SystemRoot%\System32\find.exe" /i "%~nx0" >nul && set "DBLCLK=1"

echo.
echo === USBip payload collector ===
echo   app dir      : !USBIP_APPDIR!
echo   driver store : !USBIP_STORE!
echo   system dir   : !USBIP_SYSDIR!
echo   output       : !OUT!
echo.

if exist "%OUT%" (
    echo [error] output folder already exists: !OUT!
    echo         Delete it yourself or pass another folder. This script never deletes anything.
    set "RC=2"
    goto :finish
)
mkdir "%OUT%\bin" "%OUT%\driver" "%OUT%\runtime"
if errorlevel 1 (
    echo [error] cannot create !OUT!
    set "RC=2"
    goto :finish
)

set "MISSING=0"

echo [1/3] user-mode tools
for %%F in (devnode.exe usbip.exe libusbip.dll resources.dll) do (
    if exist "%USBIP_APPDIR%\%%F" (
        copy /y "%USBIP_APPDIR%\%%F" "%OUT%\bin\" >nul
        echo   ok       bin\%%F
    ) else (
        echo   MISSING  !USBIP_APPDIR!\%%F
        set /a MISSING+=1
    )
)

echo [2/3] driver packages ^(from the driver store^)
for %%P in (usbip2_ude usbip2_filter) do (
    set "FOUND=0"
    for /d %%D in ("%USBIP_STORE%\%%P.inf_amd64_*") do (
        set /a FOUND+=1
        xcopy "%%~fD" "%OUT%\driver\%%~nxD\" /e /i /y /q >nul
        if errorlevel 1 (
            echo   FAILED   driver\%%~nxD
            set /a MISSING+=1
        ) else (
            echo   ok       driver\%%~nxD
        )
    )
    if "!FOUND!"=="0" (
        echo   MISSING  !USBIP_STORE!\%%P.inf_amd64_*
        set /a MISSING+=1
    )
    if !FOUND! GTR 1 echo   NOTE     !FOUND! versions of %%P are in the driver store; all copied - use the newest
)

echo [3/3] C++ runtime ^(the official installer puts these into System32^)
for %%F in (vcruntime140.dll vcruntime140_1.dll msvcp140.dll msvcp140_atomic_wait.dll) do (
    if exist "%USBIP_SYSDIR%\%%F" (
        copy /y "%USBIP_SYSDIR%\%%F" "%OUT%\runtime\" >nul
        echo   ok       runtime\%%F
    ) else (
        echo   MISSING  !USBIP_SYSDIR!\%%F
        set /a MISSING+=1
    )
)

rem ---- manifest: size + SHA256 + file version (certutil: avoids Get-FileHash, which is broken on machines
rem      whose PSModulePath lists PowerShell 7 module folders first) ----
set "MAN=%OUT%\MANIFEST.txt"
> "%MAN%" echo # USBip payload manifest - created %DATE% %TIME%
>> "%MAN%" echo # sha256 *relative-path   ^(size in bytes^)
for /r "%OUT%" %%F in (*) do (
    if /i not "%%~nxF"=="MANIFEST.txt" (
        set "H="
        for /f "skip=1 delims=" %%H in ('certutil -hashfile "%%F" SHA256 2^>nul') do if not defined H set "H=%%H"
        set "H=!H: =!"
        set "REL=%%F"
        set "REL=!REL:%OUT%\=!"
        >> "%MAN%" echo !H! *!REL!   ^(%%~zF^)
    )
)
>> "%MAN%" echo.
>> "%MAN%" echo # file versions ^(the official binaries were linked with MSVC 14.51: the runtime must be 14.51 or newer^)
for %%F in ("%OUT%\runtime\*.dll") do (
    for /f "usebackq delims=" %%V in (`powershell -NoProfile -Command "(Get-Item -LiteralPath '%%~fF').VersionInfo.FileVersion"`) do (
        >> "%MAN%" echo %%~nxF = %%V
        echo   version  %%~nxF = %%V
    )
)

echo.
if %MISSING% GTR 0 (
    echo [INCOMPLETE] %MISSING% item^(s^) missing - see the MISSING lines above.
    echo   Did you run the official installer first, with "Install Microsoft Visual C++ Redistributable" ticked?
    echo   Output so far: !OUT!
    set "RC=1"
) else (
    echo [OK] payload collected: !OUT!
    echo   Copy that whole folder out of the Sandbox ^(copy / paste works^) and tell Claude where you put it.
    set "RC=0"
    if not defined NO_EXPLORER start "" explorer "%OUT%"
)

:finish
echo.
if defined DBLCLK if not defined NO_PAUSE pause
exit /b %RC%
