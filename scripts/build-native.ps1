<#
.SYNOPSIS
    Builds the native components (x64 and Win32), runs the unit tests and assembles the stage
    folder that `bridge-native install` copies from (protocol/BRIDGE_NATIVE.md §4).

.EXAMPLE
    ./scripts/build-native.ps1
    ./scripts/build-native.ps1 -Configuration Debug -SkipDriver
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',

    # Skip mwbmic.sys. Implied when the WDK is not installed.
    [switch] $SkipDriver,

    [switch] $SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$nativeRoot = Join-Path $repoRoot 'native'
$outRoot = Join-Path $nativeRoot "out\$Configuration"
$kitsInclude = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'

function Get-MSBuildPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'Visual Studio 2022 (or Build Tools) is not installed: vswhere.exe was not found.'
    }
    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
        Select-Object -First 1
    if (-not $msbuild) { throw 'MSBuild was not found. Install Visual Studio Build Tools 2022 with the C++ workload.' }
    return $msbuild
}

function Assert-WindowsSdk {
    $sdk = Get-ChildItem $kitsInclude -Directory -ErrorAction SilentlyContinue |
        Where-Object { (Test-Path (Join-Path $_.FullName 'um\windows.h')) -and (Test-Path (Join-Path $_.FullName 'ucrt\stdio.h')) } |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1
    if (-not $sdk) {
        throw @'
The Windows SDK is not installed (Windows Kits\10\Include has no um\windows.h). Install it with:
  & "C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe" modify `
      --installPath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" `
      --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --passive --norestart
'@
    }
    if ([version]$sdk.Name -lt [version]'10.0.22000.0') {
        throw "Windows SDK $($sdk.Name) is too old: 10.0.22000 or newer is required (mfvirtualcamera.h)."
    }
    Write-Host "Windows SDK $($sdk.Name)"
}

function Test-Wdk {
    $kernelHeaders = Get-ChildItem $kitsInclude -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'km\wdm.h') }
    return [bool]$kernelHeaders
}

function Invoke-ProjectBuild {
    param([string] $Project, [string] $Platform)

    $path = Join-Path $nativeRoot $Project
    if (-not (Test-Path $path)) {
        Write-Warning "Skipping ${Project}: the project does not exist yet."
        return $false
    }
    Write-Host "==> $Project [$Configuration|$Platform]"
    # Out-Host keeps MSBuild's output out of the function's return value.
    & $script:msbuild $path /nologo /m /v:minimal "/p:Configuration=$Configuration" "/p:Platform=$Platform" | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $Project [$Configuration|$Platform]" }
    return $true
}

$script:msbuild = Get-MSBuildPath
Assert-WindowsSdk
if (-not (Test-Path (Join-Path $nativeRoot 'third_party\wil\include\wil\result.h'))) {
    throw 'The WIL submodule is missing: run "git submodule update --init --recursive".'
}

$built = @{
    'bridge-native' = Invoke-ProjectBuild 'bridge-native\bridge-native.vcxproj' 'x64'
    'vcam-mf'       = Invoke-ProjectBuild 'vcam-mf\vcam-mf.vcxproj' 'x64'
    'vcam-dshow-64' = Invoke-ProjectBuild 'vcam-dshow\vcam-dshow.vcxproj' 'x64'
    'vcam-dshow-32' = Invoke-ProjectBuild 'vcam-dshow\vcam-dshow.vcxproj' 'Win32'
    'tests'         = Invoke-ProjectBuild 'tests\tests.vcxproj' 'x64'
}

$buildDriver = -not $SkipDriver
if ($buildDriver -and -not (Test-Wdk)) {
    Write-Warning 'The WDK is not installed: skipping mwbmic.sys (VS component Microsoft.Windows.DriverKit).'
    $buildDriver = $false
}
if ($buildDriver) {
    $built['virtual-mic'] = Invoke-ProjectBuild 'virtual-mic\virtual-mic.vcxproj' 'x64'
}

if ($built['tests'] -and -not $SkipTests) {
    Write-Host '==> unit tests'
    & (Join-Path $outRoot 'x64\tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Unit tests failed.' }
}

$stage = Join-Path $outRoot 'stage'
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage, (Join-Path $stage 'x86'), (Join-Path $stage 'driver') | Out-Null

$layout = @(
    @{ From = 'x64\bridge-native.exe'; To = 'bridge-native.exe' }
    @{ From = 'x64\vcam-mf.dll'; To = 'vcam-mf.dll' }
    @{ From = 'x64\vcam-dshow.dll'; To = 'vcam-dshow.dll' }
    @{ From = 'Win32\vcam-dshow.dll'; To = 'x86\vcam-dshow.dll' }
    @{ From = 'x64\virtual-mic\mwbmic.sys'; To = 'driver\mwbmic.sys' }
    @{ From = 'x64\virtual-mic\mwbmic.inf'; To = 'driver\mwbmic.inf' }
    @{ From = 'x64\virtual-mic\mwbmic.cat'; To = 'driver\mwbmic.cat' }
)
$missing = @()
foreach ($item in $layout) {
    $source = Join-Path $outRoot $item.From
    if (Test-Path $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $stage $item.To) -Force
    } else {
        $missing += $item.To
    }
}

Write-Host "Stage folder: $stage"
if ($missing.Count -gt 0) {
    Write-Warning ('Not staged (component not built): ' + ($missing -join ', '))
}
