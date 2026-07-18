@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd /d D:\Projects\minview-native
cmake -B build -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% NEQ 0 exit /b 1
cmake --build build --config Release
if %ERRORLEVEL% NEQ 0 exit /b 1
copy /Y build\Release\MinView.exe MinView.exe >nul
echo BUILD_OK
