@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
cmake --build H:\GitHub\VKDemo\out\build\x64-Debug --config Debug
exit /b %errorlevel%
