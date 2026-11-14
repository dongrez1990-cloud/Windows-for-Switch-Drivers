@echo off
setlocal
set "HERE=%~dp0"
set "DEST=%LOCALAPPDATA%\SwitchWOA\SwitchPowerTray"

if exist "%DEST%\SwitchPowerTray.exe" (
    "%DEST%\SwitchPowerTray.exe" --uninstall
) else if exist "%HERE%SwitchPowerTray.exe" (
    "%HERE%SwitchPowerTray.exe" --uninstall
)

taskkill /IM SwitchPowerTray.exe /F >nul 2>nul
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v SwitchPowerTray /f >nul 2>nul
reg delete "HKCU\Software\SwitchWOA\SwitchPowerTray" /f >nul 2>nul

if /I not "%HERE:~0,-1%"=="%DEST%" (
    if exist "%DEST%" rmdir /S /Q "%DEST%" 2>nul
)

echo Restored Windows system cursors, reset Power Mode 0 and Charging Limit Off, and removed SwitchPowerTray login autostart.
exit /b 0
