@echo off
setlocal
for %%I in ("%~dp0.") do set "MINVIEW_ROOT=%%~fI"
set "MINVIEW_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "MINVIEW_CTEST_TIMEOUT_SECONDS=180"

call "%MINVIEW_VCVARS%"
if errorlevel 1 exit /b 1

cmake -S "%MINVIEW_ROOT%" -B "%MINVIEW_ROOT%\build" -A x64 -DBUILD_TESTING=ON -DMINVIEW_WARNINGS_AS_ERRORS=ON
if errorlevel 1 exit /b 1

cmake --build "%MINVIEW_ROOT%\build" --config Release -- /m
if errorlevel 1 exit /b 1

ctest --test-dir "%MINVIEW_ROOT%\build" -C Release --output-on-failure --timeout %MINVIEW_CTEST_TIMEOUT_SECONDS%
if errorlevel 1 exit /b 1

copy /Y "%MINVIEW_ROOT%\build\Release\MinView.exe" "%MINVIEW_ROOT%\MinView.exe" >nul
if errorlevel 1 exit /b 1
echo BUILD_OK
endlocal
