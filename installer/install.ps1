# install.ps1 - Chromebook Audio for Windows (Intel Alder Lake + 2x MAX98373 + NAU88L25)
#
#   -Force     install even if the hardware check fails (not recommended)
#
param([switch]$Force)

$ErrorActionPreference = 'Stop'
$here    = $PSScriptRoot
$version = (Get-Content (Join-Path $here 'VERSION.txt') -ErrorAction SilentlyContinue | Select-Object -First 1)
$regRoot = 'HKLM:\SOFTWARE\CrosSofAudio'
$progDir = Join-Path $env:ProgramFiles 'CrosSofAudio'
$svcName = 'csaudioswitcher'

function Step($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }
function Fail($m) { Write-Host "`n$m" -ForegroundColor Red; exit 1 }

Write-Host "Chromebook Audio for Windows  $version" -ForegroundColor Green

# ------------------------------------------------------------------ hardware
Step 'Checking hardware'
$present = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue
$dsp  = $present | Where-Object { $_.InstanceId -match '^PCI\\VEN_8086&DEV_(51C8|51CC)&' }
$nau  = $present | Where-Object { $_.InstanceId -match '^ACPI\\(10508825|NVTN2000|NAU8825)\\' }
$amps = @($present | Where-Object { $_.InstanceId -match '^ACPI\\(MX98373|MAX98373)\\' })

Write-Host ("  Intel Alder Lake audio DSP : {0}" -f $(if ($dsp) { $dsp[0].InstanceId.Split('&')[1] } else { 'NOT FOUND' }))
Write-Host ("  NAU88L25 headphone codec   : {0}" -f $(if ($nau) { 'found' } else { 'NOT FOUND' }))
Write-Host ("  MAX98373 speaker amps      : {0}" -f $amps.Count)

$ok = $dsp -and $nau -and ($amps.Count -ge 1)
if (-not $ok) {
    if (-not $Force) {
        Fail ("This package is for Intel Alder Lake Chromebooks with a NAU88L25 headphone codec`n" +
              "and MAX98373 speaker amps (for example the Acer Chromebook Vero 514). This laptop`n" +
              "does not have that combination, so nothing was installed.")
    }
    Write-Warning 'Hardware check failed - continuing because -Force was given.'
}
if ($amps.Count -ne 2) { Write-Warning "Expected 2 speaker amps, found $($amps.Count). Speakers may play in mono." }

# ------------------------------------------------------------------ test signing
Step 'Checking driver signing mode'
$bcd = (bcdedit /enum '{current}') -join "`n"
if ($bcd -notmatch 'testsigning\s+Yes') {
    Write-Host 'These drivers are test-signed, so Windows must be in Test Mode.' -ForegroundColor Yellow
    $a = Read-Host 'Turn Test Mode on now? (Y/N)'
    if ($a -match '^[Yy]') {
        bcdedit /set testsigning on | Out-Host
        if ($LASTEXITCODE -ne 0) {
            Fail ("Could not turn on Test Mode. Secure Boot is probably enabled - turn it off in the`n" +
                  "firmware setup (UEFI) and run this installer again.")
        }
        Fail 'Test Mode is now on. REBOOT, then run Install.cmd again.'
    }
    Fail 'Installation stopped: Test Mode is required.'
}
Write-Host '  Test Mode is on.'

# ------------------------------------------------------------------ certificate
Step 'Trusting the driver certificate'
$cer = Join-Path $here 'ChromebookAudio.cer'
certutil -addstore -f Root             $cer | Out-Null
certutil -addstore -f TrustedPublisher $cer | Out-Null
Write-Host '  done'

function Install-Inf($rel) {
    $path = Join-Path $here $rel
    if (-not (Test-Path $path)) { Write-Warning "missing $rel"; return }
    Write-Host "  $rel"
    $out = pnputil /add-driver $path /install
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010 -and $LASTEXITCODE -ne 259) {
        $out | Out-Host
        Fail "pnputil failed on $rel (code $LASTEXITCODE)"
    }
}

# ------------------------------------------------------------------ bus driver
Step 'Intel audio bus driver'
$child = $present | Where-Object { $_.InstanceId -like 'CSAUDIO\ADSP*' }
New-Item -Path $regRoot -Force | Out-Null
if ($child) {
    Write-Host '  An audio bus driver is already working on this laptop - keeping it.'
} elseif (Test-Path (Join-Path $here 'Drivers\SklHdaudBus\sklhdaudbus.inf')) {
    Install-Inf 'Drivers\SklHdaudBus\sklhdaudbus.inf'
    Set-ItemProperty -Path $regRoot -Name 'InstalledBusDriver' -Value 1 -Type DWord
    pnputil /scan-devices | Out-Null
} else {
    Write-Warning 'No audio bus driver is present and this package does not contain one.'
}

# ------------------------------------------------------------------ audio drivers
Step 'Audio drivers'
foreach ($k in 'DspBoot', 'DspIpc', 'DspDiag', 'Codec', 'AmpLeft', 'AmpRight', 'Amp0', 'Amp1') {
    if (Test-Path "$regRoot\$k") { Remove-Item "$regRoot\$k" -Recurse -Force }
}
reg add HKLM\SOFTWARE\CrosSofAudio\DspBoot /f | Out-Null
reg add HKLM\SOFTWARE\CrosSofAudio /v JackActiveLow /t REG_DWORD /d 1 /f | Out-Null
Install-Inf 'Drivers\CsAudioIntcsof\csaudiointcsof.inf'
Install-Inf 'Drivers\Nau8825Audio\Nau8825Audio.inf'
Install-Inf 'Drivers\Max98373Audio\Max98373Audio.inf'

# ------------------------------------------------------------------ switcher service
Step 'Headphone / speaker auto-switcher'
$exeSrc = Join-Path $here 'Switcher\csaudioendpointswitcher.exe'
if (Test-Path $exeSrc) {
    $svc = Get-Service -Name $svcName -ErrorAction SilentlyContinue
    if ($svc) {
        if ($svc.Status -ne 'Stopped') { Stop-Service $svcName -Force -ErrorAction SilentlyContinue }
        sc.exe delete $svcName | Out-Null
        Start-Sleep -Seconds 1
    }
    New-Item -ItemType Directory -Force $progDir | Out-Null
    Copy-Item $exeSrc $progDir -Force
    $exe = Join-Path $progDir 'csaudioendpointswitcher.exe'
    New-Service -Name $svcName -BinaryPathName "`"$exe`"" -DisplayName 'Chromebook Audio Jack Switcher' `
                -Description 'Switches between Speakers and Headphones when headphones are plugged in or out.' `
                -StartupType Automatic | Out-Null
    sc.exe failure $svcName reset= 86400 actions= restart/5000/restart/5000/restart/30000 | Out-Null
    Start-Service $svcName -ErrorAction SilentlyContinue
    Write-Host "  installed ($exe)"
} else {
    Write-Warning 'switcher not in package - skipped'
}

# ------------------------------------------------------------------ check
Step 'Result'
$amp = Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceName -match 'MAX98373' }
foreach ($a in $amp) { Write-Host "  Speaker amp driver : $($a.DriverVersion)" }
$cod = Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceName -match 'NAU88L25' }
foreach ($c in $cod) { Write-Host "  Headphone codec    : $($c.DriverVersion)" }

Write-Host "`nDone. REBOOT the laptop to start the new drivers." -ForegroundColor Green
Write-Host 'Volume tuning and troubleshooting: see README.txt.'
