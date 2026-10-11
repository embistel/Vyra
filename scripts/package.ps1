param(
    [string]$BuildDirectory = 'build',
    [string]$QtBin,
    [string]$OutDirectory = 'out'
)
# Creates a portable Windows package: out/Vyra-<version>-win64.zip
$ErrorActionPreference = 'Stop'
$projectDir = Split-Path $PSScriptRoot -Parent
$build = Resolve-Path (Join-Path $projectDir $BuildDirectory)
$version = (Select-String -Path (Join-Path $projectDir 'CMakeLists.txt') -Pattern 'project\(Vyra VERSION ([0-9.]+)').Matches[0].Groups[1].Value
if (-not $QtBin) {
    $cache = Get-Content (Join-Path $build 'CMakeCache.txt')
    $qtDir = ($cache | Select-String '^Qt6_DIR:PATH=(.*)$').Matches[0].Groups[1].Value
    $QtBin = Resolve-Path (Join-Path $qtDir '..\..\..\bin')
}
$name = "Vyra-$version-win64"
$out = Join-Path $projectDir $OutDirectory
$stage = Join-Path $out $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage, (Join-Path $stage 'licenses'), (Join-Path $stage 'translations') | Out-Null

foreach ($file in 'Vyra.exe', 'vyra-cli.exe', 'app_ko.qm', 'nvcomp64_5.dll') {
    $src = Join-Path $build $file
    if (Test-Path $src) { Copy-Item $src $stage }
    elseif ($file -ne 'nvcomp64_5.dll') { throw "missing build output: $file" }
}
& (Join-Path $QtBin 'windeployqt.exe') --release --no-translations --no-system-d3d-compiler --no-opengl-sw --compiler-runtime (Join-Path $stage 'Vyra.exe')
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed' }
# MSVC runtime next to the executables (windeployqt only copies it inside a VS developer shell).
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$crt = Get-ChildItem (Join-Path $vsRoot 'VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT') -Directory -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1
if ($crt) { Copy-Item (Join-Path $crt.FullName '*.dll') $stage }
else { Write-Warning 'MSVC runtime not found; the package needs the Visual C++ Redistributable installed.' }
$qtTranslations = Resolve-Path (Join-Path $QtBin '..\translations')
Copy-Item (Join-Path $qtTranslations 'qtbase_ko.qm') (Join-Path $stage 'translations')

Copy-Item (Join-Path $projectDir 'README.md'), (Join-Path $projectDir 'README.ko.md'), (Join-Path $projectDir 'LICENSE'), (Join-Path $projectDir 'THIRD_PARTY_NOTICES.md') $stage
$nvcomp = Join-Path $projectDir '.deps\nvidia\libnvcomp'
if (Test-Path (Join-Path $stage 'nvcomp64_5.dll')) {
    Copy-Item (Join-Path $nvcomp 'LICENSE') (Join-Path $stage 'licenses\nvCOMP-LICENSE.txt')
    Copy-Item (Join-Path $nvcomp 'NOTICE') (Join-Path $stage 'licenses\nvCOMP-NOTICE.txt')
}
$deps = Join-Path $build '_deps'
Copy-Item (Join-Path $deps 'zlibng-src\LICENSE.md') (Join-Path $stage 'licenses\zlib-ng-LICENSE.md')
Copy-Item (Join-Path $deps 'lz4-src\lib\LICENSE') (Join-Path $stage 'licenses\lz4-LICENSE.txt')
Copy-Item (Join-Path $deps 'zstd-src\LICENSE') (Join-Path $stage 'licenses\zstd-LICENSE.txt')

$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "Package: $zip"
