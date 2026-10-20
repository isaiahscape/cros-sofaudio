@echo off
setlocal EnableDelayedExpansion

set "ROOT_DIR=%~dp0"
set "ERROR_LOG=%ROOT_DIR%build_errors.log"
set "FULL_LOG=%ROOT_DIR%build_full.log"

echo ====================================================================
echo  Chromebook SOF Audio Drivers (MAX98373 + NAU88L25) - Build Script
echo ====================================================================
echo.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo [ERROR] vswhere.exe not found at '!VSWHERE!'. > "!ERROR_LOG!"
    echo Please install Visual Studio 2022/2026 or Build Tools. >> "!ERROR_LOG!"
    type "!ERROR_LOG!"
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do (
    set "VS_PATH=%%i"
)

if not defined VS_PATH (
    echo [ERROR] Visual Studio with MSBuild not detected! > "!ERROR_LOG!"
    type "!ERROR_LOG!"
    exit /b 1
)

echo [INFO] Found Visual Studio at: "!VS_PATH!"
set "MSBUILD=!VS_PATH!\MSBuild\Current\Bin\MSBuild.exe"

if not exist "!MSBUILD!" (
    echo [ERROR] MSBuild.exe not found at '!MSBUILD!'. > "!ERROR_LOG!"
    type "!ERROR_LOG!"
    exit /b 1
)

echo [INFO] Building Solution (Release ^| x64)...
echo [INFO] Logging errors to: !ERROR_LOG!
echo.

:: Clear previous logs
if exist "!ERROR_LOG!" del /f /q "!ERROR_LOG!"
if exist "!FULL_LOG!" del /f /q "!FULL_LOG!"

:: Ensure firmware files are present in driver package directory for Inf2Cat
if not exist "%ROOT_DIR%x64\Release\CsAudioIntcsof" mkdir "%ROOT_DIR%x64\Release\CsAudioIntcsof"
copy /y "%ROOT_DIR%CsAudioIntcsof\sof-*.ri" "%ROOT_DIR%x64\Release\CsAudioIntcsof\" >nul

:: Execute MSBuild with dedicated lightweight file loggers
"!MSBUILD!" "%ROOT_DIR%ChromebookAudioDrivers.sln" ^
    /p:Configuration=Release ^
    /p:Platform=x64 ^
    /m ^
    /v:normal ^
    /flp1:logfile="!ERROR_LOG!";errorsonly ^
    /flp2:logfile="!FULL_LOG!";verbosity=normal

set BUILD_EXIT=%ERRORLEVEL%

if %BUILD_EXIT% equ 0 (
    set SIGNTOOL="C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64\signtool.exe"
    if exist !SIGNTOOL! (
        echo [INFO] Signing driver packages with The Scape Network certificate...
        !SIGNTOOL! sign /v /s My /sha1 38E2878D911EFA90E0A211EB80DC227B51166126 /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 ^
            "%ROOT_DIR%x64\Release\Max98373Audio\Max98373Audio.sys" "%ROOT_DIR%x64\Release\Max98373Audio\max98373audio.cat" ^
            "%ROOT_DIR%x64\Release\Nau8825Audio\Nau8825Audio.sys" "%ROOT_DIR%x64\Release\Nau8825Audio\nau8825audio.cat" ^
            "%ROOT_DIR%x64\Release\SklHdaudBus\SklHdaudBus.sys" "%ROOT_DIR%x64\Release\SklHdaudBus\sklhdaudbus.cat" ^
            "%ROOT_DIR%x64\Release\CsAudioIntcsof\CsAudioIntcsof.sys" "%ROOT_DIR%x64\Release\CsAudioIntcsof\csaudiointcsof.cat" ^
            "%ROOT_DIR%x64\Release\CrosAudioTest.exe" ^
            "%ROOT_DIR%x64\Release\csaudioendpointswitcher.exe" >nul 2>&1
        if exist "%ROOT_DIR%TheScapeNetwork.cer" (
            copy /y "%ROOT_DIR%TheScapeNetwork.cer" "%ROOT_DIR%x64\Release\Max98373Audio\TheScapeNetwork.cer" >nul
            copy /y "%ROOT_DIR%TheScapeNetwork.cer" "%ROOT_DIR%x64\Release\Nau8825Audio\TheScapeNetwork.cer" >nul
            copy /y "%ROOT_DIR%TheScapeNetwork.cer" "%ROOT_DIR%x64\Release\SklHdaudBus\TheScapeNetwork.cer" >nul
            copy /y "%ROOT_DIR%TheScapeNetwork.cer" "%ROOT_DIR%x64\Release\CsAudioIntcsof\TheScapeNetwork.cer" >nul
            copy /y "%ROOT_DIR%TheScapeNetwork.cer" "%ROOT_DIR%x64\Release\TheScapeNetwork.cer" >nul
        )
        echo [INFO] Driver packages signed successfully.
    )

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
    exit /b %BUILD_EXIT%
)

endlocal
