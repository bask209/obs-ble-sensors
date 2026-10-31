# Builds the Release plugin and creates the installable Windows x64 ZIP.
#   powershell -ExecutionPolicy Bypass -File scripts\package.ps1
# Output: release\obs-ble-sensors-<version>-windows-x64.zip
param(
    [string] $Configuration = 'Release',
    [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Spec = Get-Content -Raw -Path (Join-Path $Root 'buildspec.json') | ConvertFrom-Json
$Name = $Spec.name
$Version = $Spec.version

Push-Location $Root
try {
    if (-not $SkipBuild) {
        cmake --preset windows-x64
        if ($LASTEXITCODE) { throw 'configure failed' }
        cmake --build build_x64 --config $Configuration --parallel
        if ($LASTEXITCODE) { throw 'build failed' }
    }

    $Stage = Join-Path $Root "release\$Configuration"
    if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
    cmake --install build_x64 --prefix $Stage --config $Configuration
    if ($LASTEXITCODE) { throw 'install failed' }

    Copy-Item (Join-Path $Root 'packaging\Install.cmd') $Stage
    Copy-Item (Join-Path $Root 'packaging\Uninstall.cmd') $Stage
    Copy-Item (Join-Path $Root 'packaging\README.txt') $Stage
    Copy-Item (Join-Path $Root 'LICENSE') (Join-Path $Stage 'LICENSE.txt')
    Copy-Item (Join-Path $Root 'NOTICE.md') (Join-Path $Stage 'NOTICE.md')

    $Zip = Join-Path $Root "release\$Name-$Version-windows-x64.zip"
    if (Test-Path $Zip) { Remove-Item -Force $Zip }
    # bsdtar (built into Windows 10+) writes portable ZIP entries ('/' separators).
    & (Join-Path $env:SystemRoot 'System32\tar.exe') -a -c -f $Zip -C $Stage obs-ble-sensors Install.cmd Uninstall.cmd README.txt LICENSE.txt NOTICE.md
    if ($LASTEXITCODE) { throw 'zip failed' }
    Write-Host "Created $Zip"

    # Setup.exe (Inno Setup 6; preinstalled on GitHub Windows runners).
    $Iscc = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $Iscc) { throw 'Inno Setup 6 (ISCC.exe) not found: winget install JRSoftware.InnoSetup' }
    & $Iscc /Q "/DAppVersion=$Version" "/DStageDir=$Stage" (Join-Path $Root 'packaging\installer.iss')
    if ($LASTEXITCODE) { throw 'installer build failed' }
    Write-Host "Created $(Join-Path $Root "release\$Name-$Version-windows-x64-setup.exe")"
} finally {
    Pop-Location
}
