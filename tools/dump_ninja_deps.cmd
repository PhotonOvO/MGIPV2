@echo off
rem Dump the Ninja deps log for one build tree so the orphan-dep audit can run
rem on any host.  Usage: tools\dump_ninja_deps.cmd <build-tree> <output-file>
setlocal EnableExtensions
if "%~1"=="" exit /b 2
if "%~2"=="" exit /b 2
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul 2>&1
if errorlevel 1 exit /b 90
cd /d "%~1"
if errorlevel 1 exit /b 91
ninja -t deps > "%~2" 2>&1
exit /b %errorlevel%
