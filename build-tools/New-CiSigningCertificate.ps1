param(
    [string]$Subject = 'CN=CrosSofAudio GitHub Actions Test',
    [ValidateRange(1, 3650)]
    [int]$ValidityDays = 30
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

$certificate = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject $Subject `
    -CertStoreLocation 'Cert:\CurrentUser\My' `
    -KeyAlgorithm RSA `
    -KeyLength 2048 `
    -HashAlgorithm SHA256 `
    -KeyExportPolicy Exportable `
    -NotAfter (Get-Date).AddDays($ValidityDays)

if (-not $certificate -or -not $certificate.HasPrivateKey) {
    throw 'Failed to create a CI code-signing certificate with a private key.'
}

$hasCodeSigningEku = $false
foreach ($extension in $certificate.Extensions) {
    if ($extension.Oid.Value -ne '2.5.29.37') { continue }
    foreach ($usage in $extension.EnhancedKeyUsages) {
        if ($usage.Value -eq '1.3.6.1.5.5.7.3.3') {
            $hasCodeSigningEku = $true
            break
        }
    }
}

if (-not $hasCodeSigningEku) {
    throw 'The generated certificate does not contain the Code Signing EKU.'
}

# Emit only the thumbprint so callers can safely capture this script's output.
Write-Output $certificate.Thumbprint.ToUpperInvariant()
