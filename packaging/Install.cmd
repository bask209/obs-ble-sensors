@echo off
setlocal
rem Installs OBS BLE Sensors for all users into the standard OBS plugin folder.
set "DEST=%ProgramData%\obs-studio\plugins"

tasklist /FI "IMAGENAME eq obs64.exe" 2>nul | find /I "obs64.exe" >nul
if not errorlevel 1 (
  echo OBS Studio is running. Please close OBS and run this installer again.
  pause
  exit /b 1
)

if not exist "%~dp0obs-ble-sensors\bin\64bit\obs-ble-sensors.dll" (
  echo Could not find the plugin files next to this installer. Extract the whole ZIP first.
  pause
  exit /b 1
)

if not exist "%DEST%" mkdir "%DEST%"
xcopy /E /I /Y /Q "%~dp0obs-ble-sensors" "%DEST%\obs-ble-sensors" >nul
if errorlevel 1 (
  echo Installation failed. Try right-click ^> "Run as administrator".
  pause
  exit /b 1
)

echo.
echo OBS BLE Sensors was installed to:
echo   %DEST%\obs-ble-sensors
echo.
echo Start OBS, then add a source: Sources ^> + ^> BLE Heart Rate
pause
