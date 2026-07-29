@echo off
setlocal

if not defined MGIF_TOOLCHAIN_SETUP set "MGIF_TOOLCHAIN_SETUP=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call "%~dp0build_debug_with_vsdevcmd.cmd" %*
exit /b %errorlevel%
