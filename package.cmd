@echo off
setlocal EnableDelayedExpansion

set "ROOT_DIR=%~dp0"
set "VERSION=v1.2.2"
if not "%~1"=="" set "VERSION=%~1"

set "DIST_DIR=%ROOT_DIR%dist\cros-sofaudio-%VERSION%"
set "ZIP_FILE=%ROOT_DIR%cros-sofaudio-%VERSION%.zip"

echo ====================================================================
echo  Chromebook SOF Audio Drivers - Packaging Release (%VERSION%)
echo ====================================================================
echo.

if not exist "%ROOT_DIR%x64\Release\Max98373Audio\Max98373Audio.sys" (
    echo [INFO] Binaries not found. Running build.cmd first...
    call "%ROOT_DIR%build.cmd"
    if !ERRORLEVEL! neq 0 (
        echo [ERROR] Build failed! Cannot package release.
        exit /b 1
    )
)

set "CERT_FILE=%ROOT_DIR%x64\Release\ChromebookAudio.cer"
if not exist "%CERT_FILE%" set "CERT_FILE=%ROOT_DIR%ChromebookAudio.cer"
if not exist "%CERT_FILE%" (
    echo [ERROR] The public certificate for the build outputs is missing.
    echo Run build.cmd with the signing certificate installed before packaging.
    exit /b 1
)

echo [INFO] Verifying package signatures and signer certificate...
set "SIGNATURE_VERIFY_POWERSHELL=powershell.exe"
if defined CSAUDIO_SIGNATURE_POWERSHELL set "SIGNATURE_VERIFY_POWERSHELL=!CSAUDIO_SIGNATURE_POWERSHELL!"
"!SIGNATURE_VERIFY_POWERSHELL!" -NoProfile -ExecutionPolicy Bypass -File "%ROOT_DIR%build-tools\Test-DriverSignatures.ps1" ^
    -CertificatePath "%CERT_FILE%" ^
    -Paths "%ROOT_DIR%x64\Release\Max98373Audio\Max98373Audio.sys;%ROOT_DIR%x64\Release\Max98373Audio\max98373audio.cat;%ROOT_DIR%x64\Release\Nau8825Audio\Nau8825Audio.sys;%ROOT_DIR%x64\Release\Nau8825Audio\nau8825audio.cat;%ROOT_DIR%x64\Release\SklHdaudBus\SklHdaudBus.sys;%ROOT_DIR%x64\Release\SklHdaudBus\sklhdaudbus.cat;%ROOT_DIR%x64\Release\CsAudioIntcsof\CsAudioIntcsof.sys;%ROOT_DIR%x64\Release\CsAudioIntcsof\csaudiointcsof.cat;%ROOT_DIR%x64\Release\CrosAudioTest.exe;%ROOT_DIR%x64\Release\csaudioendpointswitcher.exe"
if !ERRORLEVEL! neq 0 (
    echo [ERROR] Packaging stopped because the certificate does not match the signed binaries.
    exit /b 1
)

if exist "%DIST_DIR%" (
    echo [INFO] Cleaning previous dist directory...
    rmdir /s /q "%DIST_DIR%"
)
mkdir "%DIST_DIR%"

echo [INFO] Copying driver packages to Drivers\...
mkdir "%DIST_DIR%\Drivers\CsAudioIntcsof"
xcopy /y /e "%ROOT_DIR%x64\Release\CsAudioIntcsof\*" "%DIST_DIR%\Drivers\CsAudioIntcsof\" >nul
copy /y "%ROOT_DIR%CsAudioIntcsof\sof-*.ri" "%DIST_DIR%\Drivers\CsAudioIntcsof\" >nul

mkdir "%DIST_DIR%\Drivers\Max98373Audio"
xcopy /y /e "%ROOT_DIR%x64\Release\Max98373Audio\*" "%DIST_DIR%\Drivers\Max98373Audio\" >nul

mkdir "%DIST_DIR%\Drivers\Nau8825Audio"
xcopy /y /e "%ROOT_DIR%x64\Release\Nau8825Audio\*" "%DIST_DIR%\Drivers\Nau8825Audio\" >nul

mkdir "%DIST_DIR%\Drivers\SklHdaudBus"
xcopy /y /e "%ROOT_DIR%x64\Release\SklHdaudBus\*" "%DIST_DIR%\Drivers\SklHdaudBus\" >nul

echo [INFO] Copying endpoint switcher service...
mkdir "%DIST_DIR%\Switcher"
copy /y "%ROOT_DIR%x64\Release\csaudioendpointswitcher.exe" "%DIST_DIR%\Switcher\" >nul

echo [INFO] Copying diagnostic utility...
mkdir "%DIST_DIR%\Tools"
copy /y "%ROOT_DIR%x64\Release\CrosAudioTest.exe" "%DIST_DIR%\Tools\cros_audio_test.exe" >nul

echo [INFO] Copying installer scripts and documentation...
copy /y "%ROOT_DIR%installer\Install.cmd" "%DIST_DIR%\" >nul
copy /y "%ROOT_DIR%installer\install.ps1" "%DIST_DIR%\" >nul
copy /y "%ROOT_DIR%installer\Uninstall.cmd" "%DIST_DIR%\" >nul
copy /y "%ROOT_DIR%installer\uninstall.ps1" "%DIST_DIR%\" >nul
copy /y "%ROOT_DIR%installer\Collect-Logs.cmd" "%DIST_DIR%\" >nul

copy /y "%CERT_FILE%" "%DIST_DIR%\ChromebookAudio.cer" >nul

if exist "%ROOT_DIR%installer\README.txt" (
    copy /y "%ROOT_DIR%installer\README.txt" "%DIST_DIR%\" >nul
) else if exist "%ROOT_DIR%README.txt" (
    copy /y "%ROOT_DIR%README.txt" "%DIST_DIR%\" >nul
)

if exist "%ROOT_DIR%CsAudioIntcsof\LICENCE.sof-firmware" (
    copy /y "%ROOT_DIR%CsAudioIntcsof\LICENCE.sof-firmware" "%DIST_DIR%\" >nul
) else if exist "%ROOT_DIR%LICENCE.sof-firmware" (
    copy /y "%ROOT_DIR%LICENCE.sof-firmware" "%DIST_DIR%\" >nul
)

if exist "%ROOT_DIR%LICENSE" (
    copy /y "%ROOT_DIR%LICENSE" "%DIST_DIR%\" >nul
)

if exist "%ROOT_DIR%installer\VERSION.txt" (
    copy /y "%ROOT_DIR%installer\VERSION.txt" "%DIST_DIR%\" >nul
) else (
    echo %VERSION%> "%DIST_DIR%\VERSION.txt"
)

if exist "%ZIP_FILE%" del /f /q "%ZIP_FILE%"

echo [INFO] Creating release archive: %ZIP_FILE%...
powershell -NoProfile -Command "Compress-Archive -Path '%DIST_DIR%\*' -DestinationPath '%ZIP_FILE%' -CompressionLevel Optimal"

if %ERRORLEVEL% equ 0 (
    echo.
    echo ====================================================================
    echo  [SUCCESS] Release package created successfully!
    echo  Directory: %DIST_DIR%
    echo  Archive:   %ZIP_FILE%
    echo ====================================================================
) else (
    echo [ERROR] Failed to create ZIP archive!
    exit /b 1
)

endlocal
