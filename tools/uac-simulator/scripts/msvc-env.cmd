@echo off
rem msvc-env.cmd — 在 MSVC 环境中执行任意构建命令
rem 用法: msvc-env.cmd <args...>   例: msvc-env.cmd cmake --build --preset win-x64-debug
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
%*
exit /b %ERRORLEVEL%
