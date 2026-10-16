@echo off
setlocal
echo.
echo === Rock to Fakie - Installer ===
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\manage.ps1" -Action Install %*
set "RESULT=%ERRORLEVEL%"
echo.
if not "%RESULT%"=="0" echo Install failed. No unrelated game files should have been changed.
pause
exit /b %RESULT%
