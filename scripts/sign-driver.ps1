<#
.SYNOPSIS
    Test-signs a virtual-mic driver package (development only).

.DESCRIPTION
    Signs mwbmic.sys, regenerates mwbmic.cat with Inf2Cat and signs the catalog, all
    with SHA-256 and a certificate from CurrentUser\My (see scripts\make-test-cert.ps1).
    Optionally re-stamps the INF version first. Release builds are signed by Partner Center
    attestation instead; see native\virtual-mic\README.md.

.EXAMPLE
    scripts\sign-driver.ps1 -PackageDir native\out\Debug\x64\virtual-mic -Thumbprint 0123ABCD...
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $PackageDir,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Fa-f]{40}$')]
    [string] $Thumbprint,

    # Inf2Cat /os list; must cover the INF models section (NT$ARCH$.10.0...19041).
    [string] $Inf2CatOs = '10_VB_X64,10_CO_X64,10_NI_X64,10_GE_X64',

    # When set (e.g. 0.1.0.1), StampInf rewrites the INF DriverVer before cataloguing.
    [string] $StampVersion
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Find-KitTool {
    param([string] $Name, [string[]] $Architectures)

    $kitsRoot = (Get-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -Name KitsRoot10).KitsRoot10
    $versions = Get-ChildItem -Path (Join-Path $kitsRoot 'bin') -Directory -Filter '10.*' |
        Sort-Object -Property { [version]$_.Name } -Descending

    foreach ($version in $versions) {
        foreach ($arch in $Architectures) {
            $candidate = Join-Path $version.FullName (Join-Path $arch $Name)
            if (Test-Path -Path $candidate) {
                return $candidate
            }
        }
    }
    throw "$Name not found under $kitsRoot\bin. Install the Windows SDK and WDK."
}

function Invoke-Tool {
    param([string] $Path, [string[]] $Arguments)

    & $Path @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$(Split-Path -Leaf $Path) failed with exit code $LASTEXITCODE."
    }
}

$package = (Resolve-Path -Path $PackageDir).Path
$sys = Join-Path $package 'mwbmic.sys'
$inf = Join-Path $package 'mwbmic.inf'
$cat = Join-Path $package 'mwbmic.cat'

foreach ($file in @($sys, $inf)) {
    if (-not (Test-Path -Path $file)) {
        throw "Missing $file. Build the driver first."
    }
}

$cert = Get-ChildItem -Path "Cert:\CurrentUser\My\$Thumbprint" -ErrorAction SilentlyContinue
if ($null -eq $cert -or -not $cert.HasPrivateKey) {
    throw "Certificate $Thumbprint with a private key not found in CurrentUser\My."
}

$signtool = Find-KitTool -Name 'signtool.exe' -Architectures @('x64')
$inf2cat  = Find-KitTool -Name 'Inf2Cat.exe' -Architectures @('x86', 'x64')

# 1. Sign the driver binary (embedded signature; the catalog hash excludes it).
Invoke-Tool $signtool @('sign', '/q', '/fd', 'sha256', '/s', 'My', '/sha1', $Thumbprint, $sys)

# 2. Optionally re-stamp the INF, then rebuild the catalog over the signed binary.
if ($StampVersion) {
    $stampinf = Find-KitTool -Name 'stampinf.exe' -Architectures @('x64', 'x86')
    Invoke-Tool $stampinf @('-f', $inf, '-d', '*', '-v', $StampVersion, '-a', 'amd64')
}
if (Test-Path -Path $cat) {
    Remove-Item -Path $cat -Force
}
Invoke-Tool $inf2cat @("/driver:$package", "/os:$Inf2CatOs", '/uselocaltime')

# 3. Sign the catalog.
Invoke-Tool $signtool @('sign', '/q', '/fd', 'sha256', '/s', 'My', '/sha1', $Thumbprint, $cat)

Write-Host "Signed $sys and $cat with $Thumbprint."
Write-Host 'The package loads only on machines with test signing on and this certificate trusted.'
