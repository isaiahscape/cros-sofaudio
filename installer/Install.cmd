@echo off
rem Chromebook Audio (Alder Lake + MAX98373 + NAU88L25) - installer
rem Double-click this file. It asks for administrator rights itself.
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
