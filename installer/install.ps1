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

function Normalize-Thumbprint([string]$value) {
    return ($value -replace '[^0-9A-Fa-f]', '').ToUpperInvariant()
}

function Test-CodeSigningEku($certificate) {
    foreach ($extension in $certificate.Extensions) {
        if ($extension.Oid.Value -ne '2.5.29.37') { continue }
        foreach ($usage in $extension.EnhancedKeyUsages) {
            if ($usage.Value -eq '1.3.6.1.5.5.7.3.3') { return $true }
        }
    }
    return $false
}

function Add-CertificateToStore([string]$store, [string]$path) {
    $output = & certutil.exe -addstore -f $store $path 2>&1
    if ($LASTEXITCODE -ne 0) {
        $output | Out-Host
        Fail "Could not import the driver certificate into LocalMachine\$store (code $LASTEXITCODE)."
    }
}

function Test-PackageSignature([string]$path, [string]$expectedThumbprint) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Fail "The signed package file is missing: $path"
    }

    $signature = Get-AuthenticodeSignature -LiteralPath $path
    if (-not $signature.SignerCertificate) {
        Fail "The package file is not signed: $path (status: $($signature.Status))"
    }

    $actualThumbprint = Normalize-Thumbprint $signature.SignerCertificate.Thumbprint
    if ($actualThumbprint -ne $expectedThumbprint) {
        Fail ("The package contains mixed signing certificates.`n" +
              "Expected: $expectedThumbprint`nFound:    $actualThumbprint`nFile:     $path")
    }

    if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
        Fail ("Windows could not validate the signature on:`n$path`n" +
              "Status: $($signature.Status) - $($signature.StatusMessage)")
    }
}

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
$bootOptions = Get-ItemPropertyValue -Path 'HKLM:\SYSTEM\CurrentControlSet\Control' `
                                     -Name 'SystemStartOptions' -ErrorAction SilentlyContinue
$testSigningActive = (($bootOptions -split '\s+') -contains 'TESTSIGNING')
if (-not $testSigningActive) {
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
if (-not (Test-Path -LiteralPath $cer -PathType Leaf)) {
    Fail "The package signing certificate is missing: $cer"
}

$certificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($cer)
$certificateThumbprint = Normalize-Thumbprint $certificate.Thumbprint
if ((Get-Date) -lt $certificate.NotBefore -or (Get-Date) -gt $certificate.NotAfter) {
    Fail "The package signing certificate is not currently valid ($($certificate.NotBefore) through $($certificate.NotAfter))."
}
if (-not (Test-CodeSigningEku $certificate)) {
    Fail 'The package certificate is not valid for code signing.'
}

# A self-signed test certificate must be trusted as both its own root and its publisher.
if ($certificate.Subject -eq $certificate.Issuer) {
    Add-CertificateToStore 'Root' $cer
}
Add-CertificateToStore 'TrustedPublisher' $cer

$publisher = Get-Item -LiteralPath "Cert:\LocalMachine\TrustedPublisher\$certificateThumbprint" -ErrorAction SilentlyContinue
if (-not $publisher) {
    Fail "The driver certificate was not found in LocalMachine\TrustedPublisher after import."
}
if ($certificate.Subject -eq $certificate.Issuer) {
    $root = Get-Item -LiteralPath "Cert:\LocalMachine\Root\$certificateThumbprint" -ErrorAction SilentlyContinue
    if (-not $root) { Fail 'The self-signed driver certificate was not found in LocalMachine\Root after import.' }
}

foreach ($signedFile in @(
    'Drivers\SklHdaudBus\SklHdaudBus.sys',
    'Drivers\SklHdaudBus\sklhdaudbus.cat',
    'Drivers\CsAudioIntcsof\CsAudioIntcsof.sys',
    'Drivers\CsAudioIntcsof\csaudiointcsof.cat',
    'Drivers\Nau8825Audio\Nau8825Audio.sys',
    'Drivers\Nau8825Audio\nau8825audio.cat',
    'Drivers\Max98373Audio\Max98373Audio.sys',
    'Drivers\Max98373Audio\max98373audio.cat',
    'Switcher\csaudioendpointswitcher.exe',
    'Tools\cros_audio_test.exe'
)) {
    Test-PackageSignature (Join-Path $here $signedFile) $certificateThumbprint
}
Write-Host "  trusted and verified ($certificateThumbprint)"

# ------------------------------------------------------------------ code integrity policy
# These open-source drivers are test-signed and can otherwise be rejected by
# the Microsoft vulnerable-driver blocklist before PnP gets a chance to load
# them. Do this only after the complete package has passed signature checks.
Step 'Configuring Windows driver compatibility'
$ciConfigPath = 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Config'
New-Item -Path $ciConfigPath -Force | Out-Null
New-Item -Path $regRoot -Force | Out-Null

$backupCaptured = (Get-ItemProperty -Path $regRoot -Name 'BlocklistBackupCaptured' -ErrorAction SilentlyContinue).BlocklistBackupCaptured
if ($backupCaptured -ne 1) {
    $ciConfigKey = Get-Item -Path $ciConfigPath
    $previousBlocklistValue = $ciConfigKey.GetValue('VulnerableDriverBlocklistEnable', $null)
    $blocklistValueWasPresent = if ($null -eq $previousBlocklistValue) { 0 } else { 1 }
    if ($null -eq $previousBlocklistValue) { $previousBlocklistValue = 1 }

    New-ItemProperty -Path $regRoot -Name 'BlocklistValueWasPresent' -PropertyType DWord -Value $blocklistValueWasPresent -Force | Out-Null
    New-ItemProperty -Path $regRoot -Name 'PreviousVulnerableDriverBlocklistEnable' -PropertyType DWord -Value ([int]$previousBlocklistValue) -Force | Out-Null
    New-ItemProperty -Path $regRoot -Name 'BlocklistBackupCaptured' -PropertyType DWord -Value 1 -Force | Out-Null
}

New-ItemProperty -Path $ciConfigPath `
                 -Name 'VulnerableDriverBlocklistEnable' `
                 -PropertyType DWord `
                 -Value 0 `
                 -Force | Out-Null
$blocklistValue = Get-ItemPropertyValue -Path $ciConfigPath -Name 'VulnerableDriverBlocklistEnable'
if ($blocklistValue -ne 0) {
    Fail 'Windows did not accept the VulnerableDriverBlocklistEnable=0 compatibility setting.'
}
Write-Warning 'Microsoft Vulnerable Driver Blocklist disabled for these test-signed drivers. A reboot is required.'

# Keep an older switcher from reacting to PnP endpoint churn while its driver
# packages and executable are being replaced.
$existingSwitcher = Get-Service -Name $svcName -ErrorAction SilentlyContinue
if ($existingSwitcher -and $existingSwitcher.Status -ne 'Stopped') {
    Stop-Service $svcName -Force
    $existingSwitcher.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(15))
}

function Install-Inf($rel) {
    $path = Join-Path $here $rel
    if (-not (Test-Path $path)) { Fail "The package is incomplete; missing $rel" }
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
        if ($svc.Status -ne 'Stopped') {
            Stop-Service $svcName -Force
            $svc.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(15))
        }
    }
    New-Item -ItemType Directory -Force $progDir | Out-Null
    Copy-Item $exeSrc $progDir -Force
    $exe = Join-Path $progDir 'csaudioendpointswitcher.exe'
    if ($svc) {
        sc.exe config $svcName binPath= "`"$exe`"" start= auto | Out-Null
        if ($LASTEXITCODE -ne 0) { Fail "Could not update the $svcName service (code $LASTEXITCODE)." }
    } else {
        New-Service -Name $svcName -BinaryPathName "`"$exe`"" -DisplayName 'Chromebook Audio Jack Switcher' `
                    -Description 'Switches between Speakers and Headphones when headphones are plugged in or out.' `
                    -StartupType Automatic | Out-Null
    }
    sc.exe failure $svcName reset= 86400 actions= restart/5000/restart/5000/restart/30000 | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "Could not configure recovery for the $svcName service (code $LASTEXITCODE)." }
    Start-Service $svcName
    $svc = Get-Service -Name $svcName
    $svc.WaitForStatus('Running', [TimeSpan]::FromSeconds(15))
    Write-Host "  installed ($exe)"
} else {
    Fail 'The package is incomplete; the endpoint switcher is missing.'
}

# ------------------------------------------------------------------ check
Step 'Result'
$amp = Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceName -match 'MAX98373' }
foreach ($a in $amp) { Write-Host "  Speaker amp driver : $($a.DriverVersion)" }
$cod = Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceName -match 'NAU88L25' }
foreach ($c in $cod) { Write-Host "  Headphone codec    : $($c.DriverVersion)" }

Write-Host "`nDone. REBOOT the laptop to start the new drivers." -ForegroundColor Green
Write-Host 'Volume tuning and troubleshooting: see README.txt.'
