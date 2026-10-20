@echo off
setlocal EnableDelayedExpansion
title Chromebook SOF Audio Drivers Installer

:: Check for administrative rights and self-elevate if needed
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [INFO] Requesting administrator privileges...
    powershell -NoProfile -Command "Start-Process cmd -ArgumentList '/c \"\"%~f0\"\"' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"

echo ==========================================================
echo  Chromebook SOF Audio Drivers Installer (Windows 11)
echo  Maxim MAX98373 Amp + Nuvoton NAU88L25 Codec
echo  The Scape Network - Leonardo (@isaiahscape)
echo ==========================================================
echo.

set CERTPATH=%~dp0TheScapeNetwork.cer
if not exist "%CERTPATH%" set CERTPATH=%~dp0x64\Release\TheScapeNetwork.cer

set MAX_INF=%~dp0Max98373Audio\Max98373Audio.inf
if not exist "%MAX_INF%" set MAX_INF=%~dp0x64\Release\Max98373Audio\Max98373Audio.inf
if not exist "%MAX_INF%" set MAX_INF=%~dp0x64\Release\Max98373Audio.inf

set NAU_INF=%~dp0Nau8825Audio\Nau8825Audio.inf
if not exist "%NAU_INF%" set NAU_INF=%~dp0x64\Release\Nau8825Audio\Nau8825Audio.inf
if not exist "%NAU_INF%" set NAU_INF=%~dp0x64\Release\Nau8825Audio.inf

if not exist "%MAX_INF%" (
    echo [ERROR] Driver package files not found!
    echo Please make sure Max98373Audio.inf is present or run build.cmd first.
    echo.
    pause
    exit /b 1
)

:: 1. Enable test signing if needed
echo [1/4] Ensuring test signing is enabled...
bcdedit /set testsigning on >nul 2>&1

:: 2. Install security certificate
if exist "%CERTPATH%" (
    echo [2/4] Installing The Scape Network security certificate...
    certutil -addstore -f "Root" "%CERTPATH%" >nul 2>&1
    certutil -addstore -f "TrustedPublisher" "%CERTPATH%" >nul 2>&1
    echo [OK] Certificate installed into Trusted Root and Trusted Publishers.
) else (
    echo [WARNING] TheScapeNetwork.cer not found. Skipping certificate store update.
)

:: 3. Install driver packages via pnputil
echo.
echo [3/4] Installing driver packages to system...
echo Installing Maxim MAX98373 Smart Amplifier driver...
pnputil /add-driver "%MAX_INF%" /install

echo.
echo Installing Nuvoton NAU88L25 Headset Codec driver...
pnputil /add-driver "%NAU_INF%" /install

:: 4. Check hardware status
echo.
echo [4/4] Checking audio hardware device status in Device Manager...
powershell -NoProfile -Command "Get-PnpDevice | Where-Object { $_.InstanceId -like '*MX98373*' -or $_.InstanceId -like '*MAX98373*' -or $_.InstanceId -like '*10508825*' -or $_.InstanceId -like '*NVTN2000*' -or $_.InstanceId -like '*NAU8825*' } | Format-Table -Property InstanceId, Status, FriendlyName"

echo.
echo ==========================================================
echo  Installation complete!
echo ==========================================================
echo.

set TEST_EXE=%~dp0x64\Release\CrosAudioTest.exe
if not exist "%TEST_EXE%" set TEST_EXE=%~dp0CrosAudioTest.exe

if exist "%TEST_EXE%" (
    set /p RUN_TEST="Would you like to run the audio diagnostic tester now? (Y/N): "
    if /i "!RUN_TEST!"=="Y" (
        start "" "%TEST_EXE%"
    )
)

pause
