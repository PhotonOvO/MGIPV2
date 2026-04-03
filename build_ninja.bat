@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d G:\vk_minimal_latest\Demo
cmake -B out/build/x64-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C out/build/x64-debug