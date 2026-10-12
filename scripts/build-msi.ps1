param(
    [string]$BuildDirectory = 'build',
    [string]$QtBin,
    [string]$OutDirectory = 'out',
    [switch]$SkipPackage   # reuse the files staged by an earlier package.ps1 run
)
# Builds out/Vyra-<version>-x64.msi (installer/Product.wxs) from the files
# scripts/package.ps1 stages. WiX 5.0.2 is fetched from nuget.org into
# build/tools, checked against the SHA-512 in nuget.org's catalog, and runs
# on any .NET runtime 6 or newer (no .NET SDK needed).
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$projectDir = Split-Path $PSScriptRoot -Parent
$version = (Select-String -Path (Join-Path $projectDir 'CMakeLists.txt') -Pattern 'project\(Vyra VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$wixVersion = '5.0.2'
$tools = Join-Path $projectDir 'build\tools'

# NuGet package into its own directory, verified before it is extracted.
function Get-NuGetPackage([string]$Id, [string]$Version) {
    $lower = $Id.ToLowerInvariant()
    $destination = Join-Path $tools "$lower.$Version"
    $marker = Join-Path $destination 'verified.txt'
    if (Test-Path $marker) { return Join-Path $destination 'pkg' }
    $registration = Invoke-RestMethod "https://api.nuget.org/v3/registration5-gz-semver2/$lower/$Version.json"
    $catalog = Invoke-RestMethod $registration.catalogEntry
    if ($catalog.packageHashAlgorithm -ne 'SHA512') { throw "$Id ${Version}: unexpected hash algorithm $($catalog.packageHashAlgorithm)" }
    if (Test-Path $destination) { Remove-Item $destination -Recurse -Force }
    New-Item -ItemType Directory -Force $destination | Out-Null
    $nupkg = Join-Path $destination "$lower.$Version.nupkg"
    Invoke-WebRequest $registration.packageContent -OutFile $nupkg
    $sha = [System.Security.Cryptography.SHA512]::Create()
    $stream = [IO.File]::OpenRead($nupkg)
    try { $actual = [Convert]::ToBase64String($sha.ComputeHash($stream)) } finally { $stream.Close() }
    if ($actual -ne $catalog.packageHash) {
        Remove-Item $destination -Recurse -Force
        throw "$Id ${Version}: the download does not match nuget.org's SHA-512"
    }
    Expand-Archive -LiteralPath $nupkg -DestinationPath (Join-Path $destination 'pkg')
    Set-Content $marker $actual
    Write-Host "$Id $Version verified (SHA-512 from nuget.org's catalog)"
    return Join-Path $destination 'pkg'
}

$wix = Join-Path (Get-NuGetPackage 'wix' $wixVersion) 'tools\net6.0\any\wix.exe'
$uiExtension = Join-Path (Get-NuGetPackage 'WixToolset.UI.wixext' $wixVersion) 'wixext5\WixToolset.UI.wixext.dll'

if (-not $SkipPackage) {
    $arguments = @{ BuildDirectory = $BuildDirectory; OutDirectory = $OutDirectory }
    if ($QtBin) { $arguments.QtBin = $QtBin }
    & (Join-Path $PSScriptRoot 'package.ps1') @arguments
}
$out = Join-Path $projectDir $OutDirectory
$stage = Join-Path $out "Vyra-$version-win64"
if (-not (Test-Path (Join-Path $stage 'Vyra.exe'))) { throw "no staged files in $stage (run without -SkipPackage)" }
$msi = Join-Path $out "Vyra-$version-x64.msi"
$intermediate = Join-Path $projectDir 'build\msi'
New-Item -ItemType Directory -Force $intermediate | Out-Null

Push-Location $projectDir
try {
    & $wix build 'installer\Product.wxs' -arch x64 -ext $uiExtension -bindpath "Payload=$stage" `
        -define "Version=$version" -intermediatefolder $intermediate -defaultcompressionlevel high -out $msi
    if ($LASTEXITCODE -ne 0) { throw "MSI build failed ($LASTEXITCODE)" }
    & $wix msi validate $msi
    if ($LASTEXITCODE -ne 0) { throw "MSI validation failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
}
Write-Host "MSI: $msi"
