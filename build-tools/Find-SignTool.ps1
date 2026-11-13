$ErrorActionPreference = 'Stop'

$programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
$roots = @(
    (Join-Path $programFilesX86 'Windows Kits\10\bin'),
    (Join-Path $programFilesX86 'Windows Kits\11\bin')
) | Where-Object { Test-Path -LiteralPath $_ }

$requestedVersion = $env:WINDOWS_TARGET_PLATFORM_VERSION
if (-not [string]::IsNullOrWhiteSpace($requestedVersion)) {
    foreach ($root in $roots) {
        $requestedTool = Join-Path $root "$requestedVersion\x64\signtool.exe"
        if (Test-Path -LiteralPath $requestedTool -PathType Leaf) {
            Write-Output $requestedTool
            exit 0
        }
    }
    throw "signtool.exe for requested kit $requestedVersion was not found."
}

$tool = Get-ChildItem -Path $roots -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
    Sort-Object FullName |
    Select-Object -Last 1

if (-not $tool) { exit 1 }
Write-Output $tool.FullName
