@echo off
setlocal
echo.
echo === Rock to Fakie - Uninstaller ===
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\manage.ps1" -Action Uninstall %*
set "RESULT=%ERRORLEVEL%"
echo.
if not "%RESULT%"=="0" echo Uninstall stopped. Check the error above before changing files manually.
pause
exit /b %RESULT%
