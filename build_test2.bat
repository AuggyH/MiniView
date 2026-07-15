@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd /d D:\Projects\minview-native
cl /EHsc /Fe:test2.exe test_title2.cpp user32.lib /link /SUBSYSTEM:WINDOWS
echo BUILD_%ERRORLEVEL%
