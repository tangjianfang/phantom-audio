@echo off
rem build.bat - one-stop build entry for phantom-audio (repo root)
rem
rem Usage:
rem   build.bat              build Release (default) -> build\release\*.exe
rem                          then package everything needed to run into
rem                          tools\uac-simulator\dist\uacsim-sandbox-release.zip
rem   build.bat debug        build Debug + run all tests (L1+L2); if they pass, package
rem                          dist\uacsim-sandbox-debug.zip
rem   build.bat coverage     coverage build + OpenCppCoverage gate (95%)
rem   build.bat test         run tests only (requires a Debug build)
rem   build.bat clean        remove all build directories and dist\
rem   build.bat all          debug+test, release, coverage
rem
rem The zip is meant to be copied into Windows Sandbox: unzip, double-click run.cmd.
rem Set UAC_NO_PACKAGE=1 to skip packaging. See docs\sandbox.md.
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
set "SIM=%ROOT%tools\uac-simulator"
set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=release"

rem Re-enter this script inside the MSVC x64 environment. scripts\msvc-env.cmd locates Visual
rem Studio with vswhere (no hard-coded edition/year) and is the same entry CI uses.
if not defined UAC_MSVC_ENV (
    set "UAC_MSVC_ENV=1"
    call "%SIM%\scripts\msvc-env.cmd" "%~f0" %*
    exit /b !ERRORLEVEL!
)

pushd "%SIM%" || exit /b 1

if /i "%TARGET%"=="clean" goto :do_clean
if /i "%TARGET%"=="test" goto :do_test
if /i "%TARGET%"=="debug" goto :do_debug
if /i "%TARGET%"=="release" goto :do_release
if /i "%TARGET%"=="coverage" goto :do_coverage
if /i "%TARGET%"=="all" goto :do_all
echo [error] unknown target: %TARGET% (debug/release/coverage/test/clean/all)
popd & exit /b 2

:do_clean
echo === clean ===
for %%D in (debug release coverage cov-out) do if exist "build\%%D" rmdir /s /q "build\%%D"
if exist "dist" rmdir /s /q "dist"
popd & exit /b 0

:do_test
echo === run tests (L1+L2) ===
ctest --preset win-x64-debug
set "RC=!ERRORLEVEL!"
popd & exit /b !RC!

:do_debug
echo === Debug build + tests ===
cmake --preset win-x64-debug || (popd & exit /b 1)
cmake --build --preset win-x64-debug || (popd & exit /b 1)
ctest --preset win-x64-debug
set "RC=!ERRORLEVEL!"
echo.
echo output: %SIM%\build\debug\
rem Only package a build whose tests passed.
if "!RC!"=="0" (
    call :package debug
    set "RC=!ERRORLEVEL!"
)
popd & exit /b !RC!

:do_release
echo === Release build ===
cmake --preset win-x64-release || (popd & exit /b 1)
cmake --build --preset win-x64-release || (popd & exit /b 1)
echo.
echo output: %SIM%\build\release\  (uacsimd.exe / uacsim.exe / uacstudio.exe / host_probe.exe)
call :package release
set "RC=!ERRORLEVEL!"
popd & exit /b !RC!

:do_coverage
echo === coverage (requires OpenCppCoverage: winget install OpenCppCoverage.OpenCppCoverage) ===
cmake --preset win-x64-coverage || (popd & exit /b 1)
cmake --build --preset win-x64-coverage || (popd & exit /b 1)
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\coverage.ps1
set "RC=!ERRORLEVEL!"
popd & exit /b !RC!

:do_all
call "%~f0" debug || (popd & exit /b 1)
call "%~f0" release || (popd & exit /b 1)
call "%~f0" coverage
set "RC=!ERRORLEVEL!"
popd & exit /b !RC!

:package
rem Package everything needed to run into a zip for Windows Sandbox. Arg 1 = release | debug.
rem A packaging failure fails the script (non-zero exit) but never touches the build outputs.
if defined UAC_NO_PACKAGE (
    echo [package] skipped: UAC_NO_PACKAGE is set
    exit /b 0
)
echo.
echo === package for Windows Sandbox ^(%~1^) ===
powershell -NoProfile -ExecutionPolicy Bypass -File "%SIM%\scripts\package-sandbox.ps1" -BinDir "%SIM%\build\%~1"
if errorlevel 1 (
    echo [error] packaging failed - the build itself succeeded; see the message above
    exit /b 1
)
exit /b 0
