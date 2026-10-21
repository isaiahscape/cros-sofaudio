@echo off
setlocal EnableDelayedExpansion

set "ROOT_DIR=%~dp0"
set "VERSION=v1.1.7"
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

if exist "%DIST_DIR%" (
    echo [INFO] Cleaning previous dist directory...
    rmdir /s /q "%DIST_DIR%"
)
mkdir "%DIST_DIR%"

echo [INFO] Copying driver packages...
mkdir "%DIST_DIR%\CsAudioIntcsof"
xcopy /y /e "%ROOT_DIR%x64\Release\CsAudioIntcsof\*" "%DIST_DIR%\CsAudioIntcsof\" >nul

mkdir "%DIST_DIR%\Max98373Audio"
xcopy /y /e "%ROOT_DIR%x64\Release\Max98373Audio\*" "%DIST_DIR%\Max98373Audio\" >nul

mkdir "%DIST_DIR%\Nau8825Audio"
xcopy /y /e "%ROOT_DIR%x64\Release\Nau8825Audio\*" "%DIST_DIR%\Nau8825Audio\" >nul

mkdir "%DIST_DIR%\SklHdaudBus"
xcopy /y /e "%ROOT_DIR%x64\Release\SklHdaudBus\*" "%DIST_DIR%\SklHdaudBus\" >nul

echo [INFO] Copying utilities and documentation...
copy /y "%ROOT_DIR%x64\Release\csaudioendpointswitcher.exe" "%DIST_DIR%\" >nul
copy /y "%ROOT_DIR%x64\Release\CrosAudioTest.exe" "%DIST_DIR%\cros_audio_test.exe" >nul
copy /y "%ROOT_DIR%install-drivers.cmd" "%DIST_DIR%\" >nul
copy /y "%ROOT_DIR%uninstall-drivers.cmd" "%DIST_DIR%\" >nul
copy /y "%ROOT_DIR%TheScapeNetwork.cer" "%DIST_DIR%\" >nul
copy /y "%ROOT_DIR%INSTALL_GUIDE.md" "%DIST_DIR%\" >nul
copy /y "%ROOT_DIR%LICENSE" "%DIST_DIR%\" >nul
copy /y "%ROOT_DIR%README.md" "%DIST_DIR%\" >nul

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
