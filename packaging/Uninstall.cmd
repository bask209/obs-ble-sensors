@echo off
setlocal
rem Removes OBS BLE Sensors. Scenes keep their (now inactive) BLE Heart Rate sources.
set "DEST=%ProgramData%\obs-studio\plugins\obs-ble-sensors"
set "CONFIG=%APPDATA%\obs-studio\plugin_config\obs-ble-sensors"

tasklist /FI "IMAGENAME eq obs64.exe" 2>nul | find /I "obs64.exe" >nul
if not errorlevel 1 (
  echo OBS Studio is running. Please close OBS and run this again.
  pause
  exit /b 1
)

if exist "%DEST%" (
  rmdir /S /Q "%DEST%"
  if exist "%DEST%" (
    echo Could not remove %DEST%. Try right-click ^> "Run as administrator".
    pause
    exit /b 1
  )
  echo Plugin removed.
) else (
  echo The plugin is not installed in %DEST%.
)

if exist "%CONFIG%" (
  choice /M "Also delete your saved sensor list"
  if not errorlevel 2 (
    rmdir /S /Q "%CONFIG%"
    echo Saved sensor list deleted.
  )
)
pause
