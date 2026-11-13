param(
    [Parameter(Mandatory = $true)]
    [string]$SignToolPath,

    [Parameter(Mandatory = $true)]
    [string]$Thumbprint,

    [string]$StoreLocation = 'CurrentUser',

    [Parameter(Mandatory = $true)]
    [string]$Paths
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

$files = @($Paths -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
foreach ($file in $files) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Build output to sign is missing: $file"
    }
}

$arguments = @('sign', '/v')
if (-not [string]::IsNullOrWhiteSpace($env:SIGN_CERT_PFX_PATH)) {
    if (-not (Test-Path -LiteralPath $env:SIGN_CERT_PFX_PATH -PathType Leaf)) {
        throw "SIGN_CERT_PFX_PATH does not exist: $($env:SIGN_CERT_PFX_PATH)"
    }
    $arguments += @('/f', $env:SIGN_CERT_PFX_PATH)
    if (-not [string]::IsNullOrEmpty($env:SIGN_CERT_PFX_PASSWORD)) {
        $arguments += @('/p', $env:SIGN_CERT_PFX_PASSWORD)
    }
} else {
    $arguments += @('/s', 'My', '/sha1', $Thumbprint)
    if ($StoreLocation -eq 'LocalMachine') {
        $arguments += '/sm'
    }
}

$arguments += @(
    '/fd', 'SHA256',
    '/tr', 'http://timestamp.digicert.com',
    '/td', 'SHA256'
)
$arguments += $files

& $SignToolPath @arguments
if ($LASTEXITCODE -ne 0) {
    throw "SignTool failed with exit code $LASTEXITCODE."
}
