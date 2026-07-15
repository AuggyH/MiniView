@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd /d D:\Projects\minview-native
cl /EHsc /Fe:test_title.exe test_title.cpp user32.lib /link /SUBSYSTEM:WINDOWS
if %ERRORLEVEL% EQU 0 (
    echo TEST_BUILD_OK
) else (
    echo TEST_BUILD_FAIL
)
