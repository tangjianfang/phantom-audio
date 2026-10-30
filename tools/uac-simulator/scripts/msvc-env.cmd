@echo off
rem msvc-env.cmd - run any command inside an MSVC x64 environment (same entry for dev boxes and CI).
rem Usage: msvc-env.cmd <args...>      e.g. msvc-env.cmd cmake --build --preset win-x64-debug
rem
rem NOTE: keep this file ASCII-only with CRLF line endings (see .gitattributes). cmd.exe parses
rem batch files in the OEM code page, so UTF-8 comments get split into garbage tokens.
rem
rem Visual Studio is located with vswhere, so no VS year/edition is hard-coded (Community,
rem Enterprise and BuildTools all work). Without this environment CMake silently picks the
rem MinGW g++ that is on PATH (GitHub runners have one): configure "succeeds" and the build
rem fails later. CMakeLists.txt now fails fast on any non-MSVC compiler.
setlocal EnableExtensions

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo msvc-env: vswhere.exe not found - install Visual Studio with the C++ x64 toolset 1>&2
    exit /b 1
)
set "VSROOT="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT (
    echo msvc-env: no Visual Studio installation with the C++ x64 toolset found 1>&2
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo msvc-env: vcvars64.bat failed 1>&2
    exit /b 1
)

rem PATH may hit an extension-less ninja wrapper first (e.g. depot_tools\ninja); CMake then
rem treats it as the executable and fails with "inappropriate file type". Put the directory of
rem the first real ninja.exe in front.
set "NINJA_DIR="
for /f "delims=" %%N in ('where ninja.exe 2^>nul') do if not defined NINJA_DIR set "NINJA_DIR=%%~dpN"
if defined NINJA_DIR set "PATH=%NINJA_DIR%;%PATH%"

%*
exit /b %ERRORLEVEL%
