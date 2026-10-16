@echo off
setlocal
set "MODROOT=%~dp0"
set "REPO=%~1"
if not defined REPO set "REPO=%USERPROFILE%\Desktop\ReSkate-Custom"
set "BUILD=%REPO%\build\vs2026-x64"
set "DLL=%BUILD%\Release\ReSkate.dll"
echo ReSkate checkout: "%REPO%"
if not exist "%REPO%\CMakeLists.txt" (
    echo ERROR: ReSkate source checkout not found. Pass its path as the first argument.
    exit /b 1
)
if not exist "%BUILD%\CMakeCache.txt" (
    echo ERROR: Configure the ReSkate Visual Studio build tree first: "%BUILD%"
    exit /b 1
)
py -3 "%MODROOT%apply_to_reskate.py" "%REPO%" --dry-run
if errorlevel 1 (echo ERROR: Unsupported source version. & exit /b 1)
py -3 "%MODROOT%apply_to_reskate.py" "%REPO%"
if errorlevel 1 (echo ERROR: Failed to apply source. & exit /b 1)
cmake --build "%BUILD%" --config Release --target dingosdk_runtime --parallel 4
if errorlevel 1 (echo ERROR: Build failed. & exit /b 1)
if not exist "%DLL%" (echo ERROR: Compiled DLL not found at "%DLL%" & exit /b 1)
if not exist "%MODROOT%payload" mkdir "%MODROOT%payload"
copy /Y "%DLL%" "%MODROOT%payload\ReSkate.dll" >nul
if errorlevel 1 (echo ERROR: Could not stage DLL. & exit /b 1)
echo.
echo Staged: "%MODROOT%payload\ReSkate.dll"
echo Run package-release.ps1 after Windows testing to create a downloadable release.
exit /b 0
