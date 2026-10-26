@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\build_keil.ps1" -Rebuild %*
set "buildExitCode=%ERRORLEVEL%"
echo.
if not "%buildExitCode%"=="0" echo HEX build failed. Check the error above and Build\build.log.
pause
exit /b %buildExitCode%
