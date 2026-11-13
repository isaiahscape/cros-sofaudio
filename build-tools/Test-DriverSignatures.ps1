param(
    [Parameter(Mandatory = $true)]
    [string]$CertificatePath,

    [switch]$RequireValid,

    [Parameter(Mandatory = $true)]
    [string]$Paths
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
Import-Module Microsoft.PowerShell.Security -ErrorAction Stop

function Normalize-Thumbprint([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return $null }
    return ($Value -replace '[^0-9A-Fa-f]', '').ToUpperInvariant()
}

$certificatePath = (Resolve-Path -LiteralPath $CertificatePath).Path
$certificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($certificatePath)
$expectedThumbprint = Normalize-Thumbprint $certificate.Thumbprint

foreach ($path in ($Paths -split ';')) {
    if ([string]::IsNullOrWhiteSpace($path)) { continue }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Signed package file is missing: $path"
    }

    $signature = Get-AuthenticodeSignature -LiteralPath $path
    if (-not $signature.SignerCertificate) {
        throw "File is not Authenticode signed: $path (status: $($signature.Status))"
    }

    $actualThumbprint = Normalize-Thumbprint $signature.SignerCertificate.Thumbprint
    if ($actualThumbprint -ne $expectedThumbprint) {
        throw "Signer mismatch for $path. Expected $expectedThumbprint, found $actualThumbprint."
    }

    if ($signature.Status -eq [System.Management.Automation.SignatureStatus]::NotSigned -or
        $signature.Status -eq [System.Management.Automation.SignatureStatus]::HashMismatch -or
        $signature.Status -eq [System.Management.Automation.SignatureStatus]::NotSupportedFileFormat -or
        $signature.Status -eq [System.Management.Automation.SignatureStatus]::Incompatible) {
        throw "Invalid signature on $path (status: $($signature.Status); $($signature.StatusMessage))"
    }

    if ($RequireValid -and $signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
        throw "Windows does not trust the signature on $path (status: $($signature.Status); $($signature.StatusMessage))"
    }

    Write-Host "  verified $path" -ForegroundColor DarkGreen
}
