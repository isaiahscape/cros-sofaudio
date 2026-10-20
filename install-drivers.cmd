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

set BUS_INF=%~dp0SklHdaudBus\sklhdaudbus.inf
if not exist "%BUS_INF%" set BUS_INF=%~dp0x64\Release\SklHdaudBus\sklhdaudbus.inf
if not exist "%BUS_INF%" set BUS_INF=%~dp0x64\Release\sklhdaudbus.inf

set SOF_INF=%~dp0CsAudioIntcsof\csaudiointcsof.inf
if not exist "%SOF_INF%" set SOF_INF=%~dp0x64\Release\CsAudioIntcsof\csaudiointcsof.inf
if not exist "%SOF_INF%" set SOF_INF=%~dp0x64\Release\csaudiointcsof.inf

set MAX_INF=%~dp0Max98373Audio\Max98373Audio.inf
if not exist "%MAX_INF%" set MAX_INF=%~dp0x64\Release\Max98373Audio\Max98373Audio.inf
if not exist "%MAX_INF%" set MAX_INF=%~dp0x64\Release\Max98373Audio.inf

set NAU_INF=%~dp0Nau8825Audio\Nau8825Audio.inf
if not exist "%NAU_INF%" set NAU_INF=%~dp0x64\Release\Nau8825Audio\Nau8825Audio.inf
if not exist "%NAU_INF%" set NAU_INF=%~dp0x64\Release\Nau8825Audio.inf

if not exist "%MAX_INF%" (
    echo [ERROR] Driver package files not found!
    echo Please make sure driver INF files are present or run build.cmd first.
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
echo [3/4] Installing complete audio driver stack to system...
if exist "%BUS_INF%" (
    echo [1/4] Installing Intel AudioDSP Bus Driver - SklHdaudBus...
    pnputil /add-driver "%BUS_INF%" /install
)

echo [2/4] Installing Intel Sound Open Firmware WaveRT Driver - CsAudioIntcsof...
copy /y "%~dp0CsAudioIntcsof\sof-*.ri" "%SystemRoot%\System32\drivers\" >nul 2>&1
if not exist "%SystemRoot%\System32\drivers\sof-tgl.ri" (
    copy /y "%~dp0x64\Release\CsAudioIntcsof\sof-*.ri" "%SystemRoot%\System32\drivers\" >nul 2>&1
)
pnputil /add-driver "%SOF_INF%" /install

echo [3/4] Installing Maxim MAX98373 Smart Amplifier driver...
pnputil /add-driver "%MAX_INF%" /install

echo.
echo [4/4] Installing Nuvoton NAU88L25 Headset Codec driver...
pnputil /add-driver "%NAU_INF%" /install

:: 4. Deploy and start CsAudioEndpointSwitcher service
echo.
echo [*] Deploying Chromebook Audio Endpoint Switcher service...
set SWITCHER_EXE=%~dp0x64\Release\csaudioendpointswitcher.exe
if not exist "%SWITCHER_EXE%" set SWITCHER_EXE=%~dp0csaudioendpointswitcher.exe
if exist "%SWITCHER_EXE%" (
    sc stop csaudioswitcher >nul 2>&1
    timeout /t 1 /nobreak >nul
    if not exist "%ProgramFiles%\csaudiointcsof\utils" mkdir "%ProgramFiles%\csaudiointcsof\utils"
    copy /y "%SWITCHER_EXE%" "%ProgramFiles%\csaudiointcsof\utils\csaudioendpointswitcher.exe" >nul
    sc query csaudioswitcher >nul 2>&1
    if %errorLevel% neq 0 (
        sc create csaudioswitcher binPath= "\"%ProgramFiles%\csaudiointcsof\utils\csaudioendpointswitcher.exe\"" start= auto DisplayName= "csaudioswitcher" >nul
    ) else (
        sc config csaudioswitcher binPath= "\"%ProgramFiles%\csaudiointcsof\utils\csaudioendpointswitcher.exe\"" start= auto >nul
    )
    sc start csaudioswitcher >nul 2>&1
    echo [OK] Switcher service installed and running.
)

:: 4. Check hardware status
echo.
echo [4/4] Checking audio hardware device status in Device Manager...
powershell -NoProfile -Command "Get-PnpDevice | Where-Object { $_.InstanceId -like '*MX98373*' -or $_.InstanceId -like '*MAX98373*' -or $_.InstanceId -like '*10508825*' -or $_.InstanceId -like '*NVTN2000*' -or $_.InstanceId -like '*NAU8825*' -or $_.InstanceId -like '*CSAUDIO*' } | Format-Table -Property InstanceId, Status, FriendlyName"

echo.
echo ==========================================================
echo  Installation complete!
echo ==========================================================
echo.

set TEST_EXE=%~dp0x64\Release\CrosAudioTest.exe
if not exist "%TEST_EXE%" set TEST_EXE=%~dp0CrosAudioTest.exe

if exist "%TEST_EXE%" (
    set /p RUN_TEST="Would you like to run the audio diagnostic tester now? [Y/N]: "
    if /i "!RUN_TEST!"=="Y" (
        start "" "%TEST_EXE%"
    )
)

pause
