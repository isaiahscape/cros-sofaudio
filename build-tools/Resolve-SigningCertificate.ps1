param(
    [Parameter(Mandatory = $true)]
    [string]$CertificatePath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [string]$Thumbprint = $env:SIGN_CERT_SHA1
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

if (-not (Get-PSProvider -PSProvider Certificate -ErrorAction SilentlyContinue)) {
    throw 'The Windows PowerShell Certificate provider is unavailable.'
}
if (-not (Get-PSDrive -Name Cert -ErrorAction SilentlyContinue)) {
    New-PSDrive -Name Cert -PSProvider Certificate -Root '\' -Scope Global | Out-Null
}

function Normalize-Thumbprint([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return $null }
    return ($Value -replace '[^0-9A-Fa-f]', '').ToUpperInvariant()
}

function Test-CodeSigningEku($Certificate) {
    foreach ($extension in $Certificate.Extensions) {
        if ($extension.Oid.Value -ne '2.5.29.37') { continue }
        foreach ($usage in $extension.EnhancedKeyUsages) {
            if ($usage.Value -eq '1.3.6.1.5.5.7.3.3') { return $true }
        }
    }
    return $false
}

$certificatePath = (Resolve-Path -LiteralPath $CertificatePath).Path

if ([string]::IsNullOrWhiteSpace($Thumbprint)) {
    $publicCertificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($certificatePath)
    $expectedThumbprint = Normalize-Thumbprint $publicCertificate.Thumbprint
} else {
    $expectedThumbprint = Normalize-Thumbprint $Thumbprint
}

if (-not $expectedThumbprint -or $expectedThumbprint.Length -ne 40) {
    throw "The signing certificate thumbprint must contain exactly 40 hexadecimal characters."
}

$now = Get-Date
$candidate = $null
$storeLocation = $null

foreach ($location in 'CurrentUser', 'LocalMachine') {
    $item = Get-Item -LiteralPath "Cert:\$location\My\$expectedThumbprint" -ErrorAction SilentlyContinue
    if (-not $item) { continue }

    if (-not $item.HasPrivateKey) {
        throw "Certificate $expectedThumbprint exists in $location\My but has no private key."
    }
    if ($now -lt $item.NotBefore -or $now -gt $item.NotAfter) {
        throw "Certificate $expectedThumbprint is not currently valid ($($item.NotBefore) through $($item.NotAfter))."
    }

    if (-not (Test-CodeSigningEku $item)) {
        throw "Certificate $expectedThumbprint is not valid for code signing."
    }

    $candidate = $item
    $storeLocation = $location
    break
}

if (-not $candidate) {
    throw "The private key for certificate $expectedThumbprint was not found in CurrentUser\My or LocalMachine\My. Import the signing PFX, or set SIGN_CERT_SHA1 to another installed code-signing certificate."
}

$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory -and -not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}

$bytes = $candidate.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert)
[System.IO.File]::WriteAllBytes($OutputPath, $bytes)

$exportedCertificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($OutputPath)
if ((Normalize-Thumbprint $exportedCertificate.Thumbprint) -ne $expectedThumbprint) {
    throw "The exported certificate does not match the selected signing certificate."
}

# Machine-readable output consumed by build.cmd.
Write-Output "$storeLocation|$expectedThumbprint"
