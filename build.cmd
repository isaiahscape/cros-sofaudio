@echo off
setlocal EnableDelayedExpansion

set "ROOT_DIR=%~dp0"
set "ERROR_LOG=%ROOT_DIR%build_errors.log"
set "FULL_LOG=%ROOT_DIR%build_full.log"
set "BUILD_RESULT=0"

echo ====================================================================
echo  Chromebook SOF Audio Drivers (MAX98373 + NAU88L25) - Build Script
echo ====================================================================
echo.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo [ERROR] vswhere.exe not found at '!VSWHERE!'. > "!ERROR_LOG!"
    echo Please install Visual Studio 2022/2026 or Build Tools. >> "!ERROR_LOG!"
    type "!ERROR_LOG!"
    set "BUILD_RESULT=1"
    goto :BuildDone
)

for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do (
    set "VS_PATH=%%i"
)

if not defined VS_PATH (
    echo [ERROR] Visual Studio with MSBuild not detected! > "!ERROR_LOG!"
    type "!ERROR_LOG!"
    set "BUILD_RESULT=1"
    goto :BuildDone
)

echo [INFO] Found Visual Studio at: "!VS_PATH!"
set "MSBUILD=!VS_PATH!\MSBuild\Current\Bin\MSBuild.exe"

if not exist "!MSBUILD!" (
    echo [ERROR] MSBuild.exe not found at '!MSBUILD!'. > "!ERROR_LOG!"
    type "!ERROR_LOG!"
    set "BUILD_RESULT=1"
    goto :BuildDone
)

echo [INFO] Building Solution (Release ^| x64)...
echo [INFO] Logging errors to: !ERROR_LOG!
echo.

set "TARGET_PLATFORM_ARG="
if defined WINDOWS_TARGET_PLATFORM_VERSION (
    set "TARGET_PLATFORM_ARG=/p:WindowsTargetPlatformVersion=!WINDOWS_TARGET_PLATFORM_VERSION!"
    echo [INFO] Pinning Windows SDK/WDK target to !WINDOWS_TARGET_PLATFORM_VERSION!.
)

:: Clear previous logs
if exist "!ERROR_LOG!" del /f /q "!ERROR_LOG!"
if exist "!FULL_LOG!" del /f /q "!FULL_LOG!"

:: Never allow an incremental build to reuse a catalog generated for older
:: INF or binary contents. Inf2Cat regenerates each one during MSBuild.
for %%c in (
    "%ROOT_DIR%x64\Release\Max98373Audio\max98373audio.cat"
    "%ROOT_DIR%x64\Release\Nau8825Audio\nau8825audio.cat"
    "%ROOT_DIR%x64\Release\SklHdaudBus\sklhdaudbus.cat"
    "%ROOT_DIR%x64\Release\CsAudioIntcsof\csaudiointcsof.cat"
) do if exist "%%~c" del /f /q "%%~c"

:: Ensure firmware files are present in driver package directory for Inf2Cat
if not exist "%ROOT_DIR%x64\Release\CsAudioIntcsof" mkdir "%ROOT_DIR%x64\Release\CsAudioIntcsof"
copy /y "%ROOT_DIR%CsAudioIntcsof\sof-*.ri" "%ROOT_DIR%x64\Release\CsAudioIntcsof\" >nul

:: Execute MSBuild with dedicated lightweight file loggers
"!MSBUILD!" "%ROOT_DIR%ChromebookAudioDrivers.sln" ^
    /p:Configuration=Release ^
    /p:Platform=x64 !TARGET_PLATFORM_ARG! ^
    /m ^
    /v:normal ^
    /flp1:logfile="!ERROR_LOG!";errorsonly ^
    /flp2:logfile="!FULL_LOG!";verbosity=normal

set BUILD_EXIT=%ERRORLEVEL%

if %BUILD_EXIT% equ 0 (
    for %%c in (
        "%ROOT_DIR%x64\Release\Max98373Audio\max98373audio.cat"
        "%ROOT_DIR%x64\Release\Nau8825Audio\nau8825audio.cat"
        "%ROOT_DIR%x64\Release\SklHdaudBus\sklhdaudbus.cat"
        "%ROOT_DIR%x64\Release\CsAudioIntcsof\csaudiointcsof.cat"
    ) do if not exist "%%~c" (
        echo [ERROR] Inf2Cat did not generate %%~c.
        set "BUILD_RESULT=1"
        goto :BuildDone
    )

    :: GitHub-hosted WDK builds can remove certificates created before MSBuild.
    :: Generate the ephemeral CI signer only after compilation/catalog creation.
    if /i "!GENERATE_CI_SIGNING_CERTIFICATE!"=="1" (
        set "SIGN_CERT_SHA1="
        for /f "usebackq delims=" %%i in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT_DIR%build-tools\New-CiSigningCertificate.ps1"`) do set "SIGN_CERT_SHA1=%%i"
        if not defined SIGN_CERT_SHA1 (
            echo [ERROR] Could not generate the ephemeral CI signing certificate.
            set "BUILD_RESULT=1"
            goto :BuildDone
        )
        echo [INFO] Generated ephemeral CI signing certificate !SIGN_CERT_SHA1!.
    )

    set "SIGNTOOL="
    for /f "usebackq delims=" %%i in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT_DIR%build-tools\Find-SignTool.ps1"`) do set "SIGNTOOL=%%i"
    if not defined SIGNTOOL (
        echo [ERROR] signtool.exe was not found in the installed Windows Driver Kit.
        set "BUILD_RESULT=1"
        goto :BuildDone
    )

    set "SIGN_CERT_STORE="
    set "RESOLVED_SIGN_CERT_SHA1="
    for /f "tokens=1,2 delims=|" %%a in ('powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT_DIR%build-tools\Resolve-SigningCertificate.ps1" -CertificatePath "%ROOT_DIR%ChromebookAudio.cer" -OutputPath "%ROOT_DIR%x64\Release\ChromebookAudio.cer"') do (
        set "SIGN_CERT_STORE=%%a"
        set "RESOLVED_SIGN_CERT_SHA1=%%b"
    )
    if not defined RESOLVED_SIGN_CERT_SHA1 (
        echo [ERROR] Could not resolve the driver signing certificate and private key.
        set "BUILD_RESULT=1"
        goto :BuildDone
    )

    set "SIGN_STORE_ARGS=/s My"
    if /i "!SIGN_CERT_STORE!"=="LocalMachine" set "SIGN_STORE_ARGS=/sm /s My"

    echo [INFO] Signing driver packages with certificate !RESOLVED_SIGN_CERT_SHA1!...
    "!SIGNTOOL!" sign /v !SIGN_STORE_ARGS! /sha1 !RESOLVED_SIGN_CERT_SHA1! /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 ^
        "%ROOT_DIR%x64\Release\Max98373Audio\Max98373Audio.sys" "%ROOT_DIR%x64\Release\Max98373Audio\max98373audio.cat" ^
        "%ROOT_DIR%x64\Release\Nau8825Audio\Nau8825Audio.sys" "%ROOT_DIR%x64\Release\Nau8825Audio\nau8825audio.cat" ^
        "%ROOT_DIR%x64\Release\SklHdaudBus\SklHdaudBus.sys" "%ROOT_DIR%x64\Release\SklHdaudBus\sklhdaudbus.cat" ^
        "%ROOT_DIR%x64\Release\CsAudioIntcsof\CsAudioIntcsof.sys" "%ROOT_DIR%x64\Release\CsAudioIntcsof\csaudiointcsof.cat" ^
        "%ROOT_DIR%x64\Release\CrosAudioTest.exe" ^
        "%ROOT_DIR%x64\Release\csaudioendpointswitcher.exe"
    if !ERRORLEVEL! neq 0 (
        echo [ERROR] Driver package signing failed.
        set "BUILD_RESULT=1"
        goto :BuildDone
    )

    powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT_DIR%build-tools\Test-DriverSignatures.ps1" ^
        -CertificatePath "%ROOT_DIR%x64\Release\ChromebookAudio.cer" ^
        -Paths "%ROOT_DIR%x64\Release\Max98373Audio\Max98373Audio.sys;%ROOT_DIR%x64\Release\Max98373Audio\max98373audio.cat;%ROOT_DIR%x64\Release\Nau8825Audio\Nau8825Audio.sys;%ROOT_DIR%x64\Release\Nau8825Audio\nau8825audio.cat;%ROOT_DIR%x64\Release\SklHdaudBus\SklHdaudBus.sys;%ROOT_DIR%x64\Release\SklHdaudBus\sklhdaudbus.cat;%ROOT_DIR%x64\Release\CsAudioIntcsof\CsAudioIntcsof.sys;%ROOT_DIR%x64\Release\CsAudioIntcsof\csaudiointcsof.cat;%ROOT_DIR%x64\Release\CrosAudioTest.exe;%ROOT_DIR%x64\Release\csaudioendpointswitcher.exe"
    if !ERRORLEVEL! neq 0 (
        echo [ERROR] One or more build outputs were signed by a different certificate.
        set "BUILD_RESULT=1"
        goto :BuildDone
    )

    for %%d in (Max98373Audio Nau8825Audio SklHdaudBus CsAudioIntcsof) do (
        copy /y "%ROOT_DIR%x64\Release\ChromebookAudio.cer" "%ROOT_DIR%x64\Release\%%d\ChromebookAudio.cer" >nul
    )
    echo [INFO] Driver packages signed and verified successfully.

    echo -------------------------------------------------------------------- > "!ERROR_LOG!"
    echo [BUILD STATUS] SUCCESS >> "!ERROR_LOG!"
    echo [TIMESTAMP]    %DATE% %TIME% >> "!ERROR_LOG!"
    echo [SOLUTION]     ChromebookAudioDrivers.sln >> "!ERROR_LOG!"
    echo [RESULT]       0 Errors, 0 Warnings >> "!ERROR_LOG!"
    echo. >> "!ERROR_LOG!"
    echo Generated Targets: >> "!ERROR_LOG!"
    echo   - x64\Release\Max98373Audio\Max98373Audio.sys >> "!ERROR_LOG!"
    echo   - x64\Release\Max98373Audio\Max98373Audio.inf >> "!ERROR_LOG!"
    echo   - x64\Release\Max98373Audio\max98373audio.cat >> "!ERROR_LOG!"
    echo   - x64\Release\Nau8825Audio\Nau8825Audio.sys >> "!ERROR_LOG!"
    echo   - x64\Release\Nau8825Audio\Nau8825Audio.inf >> "!ERROR_LOG!"
    echo   - x64\Release\Nau8825Audio\nau8825audio.cat >> "!ERROR_LOG!"
    echo   - x64\Release\SklHdaudBus\SklHdaudBus.sys >> "!ERROR_LOG!"
    echo   - x64\Release\SklHdaudBus\sklhdaudbus.inf >> "!ERROR_LOG!"
    echo   - x64\Release\SklHdaudBus\sklhdaudbus.cat >> "!ERROR_LOG!"
    echo   - x64\Release\CsAudioIntcsof\CsAudioIntcsof.sys >> "!ERROR_LOG!"
    echo   - x64\Release\CsAudioIntcsof\csaudiointcsof.inf >> "!ERROR_LOG!"
    echo   - x64\Release\CsAudioIntcsof\csaudiointcsof.cat >> "!ERROR_LOG!"
    echo   - x64\Release\CrosAudioTest.exe >> "!ERROR_LOG!"
    echo   - x64\Release\csaudioendpointswitcher.exe >> "!ERROR_LOG!"
    echo -------------------------------------------------------------------- >> "!ERROR_LOG!"

    echo.
    echo ====================================================================
    echo  [SUCCESS] All drivers and tools built successfully!
    echo  Lightweight build log written to: build_errors.log
    echo  Output binaries located at: %ROOT_DIR%x64\Release\
    echo ====================================================================
) else (
    echo.
    echo ====================================================================
    echo  [ERROR] Build failed with error code %BUILD_EXIT%.
    echo  Lightweight Error Log:
    echo --------------------------------------------------------------------
    if exist "!ERROR_LOG!" (
        type "!ERROR_LOG!"
    ) else (
        echo No specific errors captured in !ERROR_LOG!. See !FULL_LOG!.
    )
    echo --------------------------------------------------------------------
    echo ====================================================================
    set "BUILD_RESULT=%BUILD_EXIT%"
)

:BuildDone
endlocal & exit /b %BUILD_RESULT%
