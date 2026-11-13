param(
    [Parameter(Mandatory = $true)]
    [string]$PfxPath,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedCertificatePath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [string]$Password = $env:SIGN_CERT_PFX_PASSWORD
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

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

$pfxPath = (Resolve-Path -LiteralPath $PfxPath).Path
$expectedPath = (Resolve-Path -LiteralPath $ExpectedCertificatePath).Path
$flags = [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet
$certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
    $pfxPath,
    $Password,
    $flags
)

if (-not $certificate.HasPrivateKey) {
    throw 'The supplied PFX does not contain an accessible private key.'
}
if ((Get-Date) -lt $certificate.NotBefore -or (Get-Date) -gt $certificate.NotAfter) {
    throw "The PFX signing certificate is not currently valid ($($certificate.NotBefore) through $($certificate.NotAfter))."
}
if (-not (Test-CodeSigningEku $certificate)) {
    throw 'The PFX certificate does not contain the Code Signing EKU.'
}

$expectedCertificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($expectedPath)
$actualThumbprint = Normalize-Thumbprint $certificate.Thumbprint
$expectedThumbprint = Normalize-Thumbprint $expectedCertificate.Thumbprint
if ($actualThumbprint -ne $expectedThumbprint) {
    throw "The PFX does not match $ExpectedCertificatePath. Expected $expectedThumbprint, found $actualThumbprint."
}

$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory -and -not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}
[System.IO.File]::WriteAllBytes(
    $OutputPath,
    $certificate.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert)
)

Write-Output $actualThumbprint
