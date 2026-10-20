@echo off
setlocal EnableDelayedExpansion
title Chromebook SOF Audio Drivers Uninstaller

:: Check for administrative rights and self-elevate if needed
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [INFO] Requesting administrator privileges...
    powershell -NoProfile -Command "Start-Process cmd -ArgumentList '/c \"\"%~f0\"\"' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"

echo ==========================================================
echo  Chromebook SOF Audio Drivers Uninstaller (Windows 11)
echo  The Scape Network - Leonardo (@isaiahscape)
echo ==========================================================
echo.

:: 1. Stop and remove CsAudioEndpointSwitcher service
echo [1/4] Stopping and removing Chromebook Audio Endpoint Switcher service...
sc stop csaudioswitcher >nul 2>&1
taskkill /f /im csaudioendpointswitcher.exe >nul 2>&1
sc delete csaudioswitcher >nul 2>&1

if exist "%ProgramFiles%\csaudiointcsof" (
    rmdir /s /q "%ProgramFiles%\csaudiointcsof" >nul 2>&1
)
echo [OK] Switcher service and files removed.

:: 2. Uninstall driver packages from Windows Driver Store
echo.
echo [2/4] Uninstalling driver packages from Windows Driver Store...
powershell -NoProfile -Command "
    \$drivers = pnputil /enum-drivers
    \$pattern = 'csaudiointcsof\.inf|max98373audio\.inf|nau8825audio\.inf|sklhdaudbus\.inf'
    \$lines = \$drivers -split \"`r?`n\"
    \$curOem = ''
    foreach (\$line in \$lines) {
        if (\$line -match 'Published Name:\s+(oem\d+\.inf)') {
            \$curOem = \$matches[1]
        }
        if (\$line -match 'Original Name:\s+(' + \$pattern + ')') {
            if (\$curOem) {
                Write-Host \"[INFO] Removing \$curOem (\$($matches[1]))...\"
                pnputil /delete-driver \$curOem /uninstall /force
                \$curOem = ''
            }
        }
    }
"

:: 3. Remove copied DSP firmware files
echo.
echo [3/4] Removing DSP firmware files from System32\drivers...
if exist "%SystemRoot%\System32\drivers\sof-*.ri" (
    del /f /q "%SystemRoot%\System32\drivers\sof-*.ri" >nul 2>&1
    echo [OK] Removed sof-*.ri firmware files.
) else (
    echo [INFO] No sof-*.ri firmware files found.
)

:: 4. Rescan hardware devices
echo.
echo [4/4] Rescanning hardware devices for Windows fallback drivers...
pnputil /scan-devices >nul 2>&1
echo [OK] Hardware scan complete.

echo.
echo ==========================================================
echo  [SUCCESS] Chromebook SOF Audio Drivers Uninstalled.
echo  A system reboot is recommended to finish driver cleanup.
echo ==========================================================
echo.
pause
