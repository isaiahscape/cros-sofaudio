@echo off
rem Saves a diagnostic log (driver state, DSP, codec, amps, switcher) next to this file
rem and into Desktop\acer volmar if that folder exists.
set "LOG=%~dp0audio-log.txt"
(
  echo == DSP boot ==
  reg query HKLM\SOFTWARE\CrosSofAudio\DspBoot
  echo.
  echo == DSP IPC / pipelines ==
  reg query HKLM\SOFTWARE\CrosSofAudio\DspIpc
  echo.
  echo == Data-flow snapshots ==
  reg query HKLM\SOFTWARE\CrosSofAudio\DspDiag
  echo.
  echo == Speaker amps ==
  reg query HKLM\SOFTWARE\CrosSofAudio\AmpLeft
  reg query HKLM\SOFTWARE\CrosSofAudio\AmpRight
  reg query HKLM\SOFTWARE\CrosSofAudio\Amp0
  reg query HKLM\SOFTWARE\CrosSofAudio\Amp1
  echo.
  echo == Headphone codec / jack ==
  reg query HKLM\SOFTWARE\CrosSofAudio\Codec
  echo.
  echo == Settings ==
  reg query HKLM\SOFTWARE\CrosSofAudio /v *Atten*
  reg query HKLM\SOFTWARE\CrosSofAudio /v JackActiveLow
  echo.
  echo == Switcher service ==
  sc query csaudioswitcher
  powershell -NoProfile -Command "if (Test-Path \"$env:ProgramData\CrosSofAudio\switcher.log\") { Get-Content \"$env:ProgramData\CrosSofAudio\switcher.log\" -Tail 40 }"
  echo.
  echo == Drivers ==
  powershell -NoProfile -Command "Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.InfName -and $_.DeviceName -match 'SOF Audio|Nuvoton|MAX98373|HD Audio' } | Format-List DeviceName,DriverVersion,InfName"
  echo == Audio endpoints ==
  powershell -NoProfile -Command "Get-PnpDevice | Where-Object { $_.Class -eq 'AudioEndpoint' -and $_.FriendlyName -match 'Chromebook' } | Format-Table -AutoSize Status,FriendlyName | Out-String -Width 300"
) > "%LOG%" 2>&1
if exist "%USERPROFILE%\Desktop\acer volmar" copy /y "%LOG%" "%USERPROFILE%\Desktop\acer volmar\audio-log.txt" >nul
type "%LOG%"
echo.
echo Saved to %LOG%
pause
