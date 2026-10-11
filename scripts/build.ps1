param(
    [string]$Architecture = 'native',
    [string]$QtRoot = $env:QT_ROOT,
    [switch]$InstallDependencies,
    [switch]$NoCuda,
    [switch]$Test,
    [string]$BuildDirectory = 'build'
)
$ErrorActionPreference = 'Stop'
$projectDir = Split-Path $PSScriptRoot -Parent
if ($InstallDependencies -and -not $NoCuda) {
    # nvCOMP is distributed as a pip wheel; install into the project-local .deps folder only.
    & python -m pip install --target "$projectDir\.deps" --no-deps nvidia-libnvcomp-cu13==5.3.0.16
    if ($LASTEXITCODE -ne 0) { throw 'nvCOMP installation failed' }
}
if (-not $QtRoot) {
    $candidate = Get-ChildItem 'C:\Qt', 'D:\Qt' -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^6\.' } | Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'msvc2022_64' } | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $candidate) { throw 'Qt 6 (msvc2022_64) not found. Pass -QtRoot or set QT_ROOT.' }
    $QtRoot = $candidate
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
# CUDA 13 supports Visual Studio 2022 (v17); prefer it over newer previews.
$vsRoot = & $vswhere -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vsRoot) { $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
if (-not $vsRoot) { throw 'Visual Studio C++ build tools not found' }
Import-Module "$vsRoot\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$cmakeBin = "$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$ninjaExe = "$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$buildDir = Join-Path $projectDir $BuildDirectory
$cudaArgs = if ($NoCuda) { @('-DVYRA_WITH_CUDA=OFF') } else { @("-DCMAKE_CUDA_ARCHITECTURES=$Architecture") }
& "$cmakeBin\cmake.exe" -S $projectDir -B $buildDir -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninjaExe" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$QtRoot" -DBUILD_TESTING=ON @cudaArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& "$cmakeBin\cmake.exe" --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
if ($Test) {
    & "$cmakeBin\ctest.exe" --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
}
