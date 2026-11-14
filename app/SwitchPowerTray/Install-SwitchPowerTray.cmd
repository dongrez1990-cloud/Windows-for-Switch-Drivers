@echo off
setlocal
set "SRC=%~dp0"
set "DEST=%LOCALAPPDATA%\SwitchWOA\SwitchPowerTray"

if not exist "%SRC%SwitchPowerTray.exe" (
    echo ERROR: SwitchPowerTray.exe not found next to this script.
    exit /b 1
)

if exist "%DEST%\SwitchPowerTray.exe" (
    "%DEST%\SwitchPowerTray.exe" --restore-cursors
    taskkill /IM SwitchPowerTray.exe /F >nul 2>nul
)

mkdir "%DEST%" 2>nul
copy /Y "%SRC%SwitchPowerTray.exe" "%DEST%\SwitchPowerTray.exe" >nul || exit /b 1
copy /Y "%SRC%Restore-SwitchPowerTray.cmd" "%DEST%\Restore-SwitchPowerTray.cmd" >nul || exit /b 1

"%DEST%\SwitchPowerTray.exe" --install-autostart || exit /b 1
start "" "%DEST%\SwitchPowerTray.exe" --minimized

echo Installed SwitchPowerTray to "%DEST%" and enabled Start at Login.
echo Default platform clocks remain unchanged (CPU 1497.6 MHz, GPU 460.8 MHz, RAM 1600 MHz, Charging Limit Off).
exit /b 0
