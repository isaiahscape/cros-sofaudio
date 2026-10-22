# uninstall.ps1 - removes Chromebook Audio for Windows
$ErrorActionPreference = 'Continue'
$regRoot = 'HKLM:\SOFTWARE\CrosSofAudio'
$progDir = Join-Path $env:ProgramFiles 'CrosSofAudio'
$svcName = 'csaudioswitcher'

Write-Host 'Removing the auto-switcher service...'
$svc = Get-Service -Name $svcName -ErrorAction SilentlyContinue
if ($svc) {
    Stop-Service $svcName -Force -ErrorAction SilentlyContinue
    sc.exe delete $svcName | Out-Null
}
if (Test-Path $progDir) { Remove-Item $progDir -Recurse -Force -ErrorAction SilentlyContinue }

$names = @('csaudiointcsof', 'nau8825audio', 'max98373audio')
$busInstalled = (Get-ItemProperty -Path $regRoot -Name 'InstalledBusDriver' -ErrorAction SilentlyContinue).InstalledBusDriver -eq 1
if ($busInstalled) { $names += 'sklhdaudbus' }

$text = (pnputil /enum-drivers) -join "`n"
$blocks = $text -split "(\r?\n){2,}"
$targets = @()
foreach ($b in $blocks) {
    foreach ($n in $names) {
        if ($b -match "Original Name:\s+$n\.inf" -and $b -match '18\.[23]\.\d+\.\d+') {
            if ($b -match 'Published Name:\s+(oem\d+\.inf)') { $targets += $Matches[1] }
        }
    }
}
if (-not $targets) {
    Write-Host 'No Chromebook Audio driver packages (18.2.x / 18.3.x) found.'
    Write-Host 'If Windows is not in English, use Device Manager -> Uninstall device instead.'
} else {
    foreach ($t in $targets) {
        Write-Host "Removing $t ..."
        pnputil /delete-driver $t /uninstall /force | Out-Host
    }
    pnputil /scan-devices | Out-Null
}
if (Test-Path $regRoot) {
    foreach ($k in 'DspBoot', 'DspIpc', 'DspDiag', 'Codec', 'AmpLeft', 'AmpRight', 'Amp0', 'Amp1') {
        if (Test-Path "$regRoot\$k") { Remove-Item "$regRoot\$k" -Recurse -Force }
    }
    Remove-ItemProperty -Path $regRoot -Name 'InstalledBusDriver' -ErrorAction SilentlyContinue
}
Write-Host "`nDone. Reboot to finish. Windows goes back to any older audio driver it still has." -ForegroundColor Green
