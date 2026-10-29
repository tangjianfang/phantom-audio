@echo off
rem build.bat - one-stop build entry for phantom-audio (repo root)
rem
rem Usage:
rem   build.bat              build Release (default) -> build\release\*.exe
rem   build.bat debug        build Debug + run all tests (L1+L2)
rem   build.bat coverage     coverage build + OpenCppCoverage gate (95%)
rem   build.bat test         run tests only (requires a Debug build)
rem   build.bat clean        remove all build directories
rem   build.bat all          debug+test, release, coverage
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
set "SIM=%ROOT%tools\uac-simulator"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=release"

if not exist "%VCVARS%" (
    echo [error] VS2022 vcvars64 not found: %VCVARS%
    exit /b 1
)
call "%VCVARS%" >nul 2>&1 || exit /b 1

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
popd & exit /b !RC!

:do_release
echo === Release build ===
cmake --preset win-x64-release || (popd & exit /b 1)
cmake --build --preset win-x64-release || (popd & exit /b 1)
echo.
echo output: %SIM%\build\release\  (uacsimd.exe / uacsim.exe / uacstudio.exe / host_probe.exe)
popd & exit /b 0

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
