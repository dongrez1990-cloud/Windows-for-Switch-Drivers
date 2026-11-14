@echo off
setlocal
set "HERE=%~dp0"
set "DEST=%LOCALAPPDATA%\SwitchWOA\SwitchPowerTray"

if exist "%DEST%\SwitchPowerTray.exe" (
    "%DEST%\SwitchPowerTray.exe" --uninstall
) else if exist "%HERE%SwitchPowerTray.exe" (
    "%HERE%SwitchPowerTray.exe" --uninstall
) else (
    echo ERROR: SwitchPowerTray.exe is missing; driver defaults were not restored.
    exit /b 2
)
if errorlevel 1 (
    echo ERROR: Restore failed. Keeping the program and recovery settings for retry.
    exit /b 1
)

taskkill /IM SwitchPowerTray.exe /F >nul 2>nul
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v SwitchPowerTray /f >nul 2>nul
reg delete "HKCU\Software\SwitchWOA\SwitchPowerTray" /f >nul 2>nul

if /I not "%HERE:~0,-1%"=="%DEST%" (
    if exist "%DEST%" rmdir /S /Q "%DEST%" 2>nul
)

echo Restored cursors and removed login autostart. Drivers accepted Windows baseline Mode 7 and Charging Limit Off.
echo GPU clock transition is asynchronous; verify actual clock readback before declaring hardware restoration complete.
exit /b 0
