<#
.SYNOPSIS
    Creates (or reuses) a code-signing test certificate for development builds of mwbmic.sys.

.DESCRIPTION
    The certificate lives in CurrentUser\My with a non-exportable private key; only the public
    part is exported (.cer) for the test machine. Test-signed drivers load only where test signing
    is enabled (bcdedit /set testsigning on, Secure Boot off) and the certificate is trusted, i.e.
    in a development VM. Never install this certificate on a machine you care about.

.EXAMPLE
    scripts\make-test-cert.ps1
    $env:MWB_TEST_CERT_THUMBPRINT = '<printed thumbprint>'
#>
[CmdletBinding()]
param(
    [string] $Subject = 'CN=MobileWebcamBridge Test Driver Signing',
    [string] $OutFile = (Join-Path $PSScriptRoot '..\native\out\MobileWebcamBridgeTest.cer'),
    [ValidateRange(1, 10)]
    [int] $ValidYears = 2
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$cert = Get-ChildItem -Path Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq $Subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(30) } |
    Sort-Object -Property NotAfter -Descending |
    Select-Object -First 1

if ($null -ne $cert) {
    Write-Host "Reusing existing test certificate $($cert.Thumbprint) (expires $($cert.NotAfter.ToString('yyyy-MM-dd')))."
}
else {
    $cert = New-SelfSignedCertificate `
        -Subject $Subject `
        -Type CodeSigningCert `
        -CertStoreLocation Cert:\CurrentUser\My `
        -KeyAlgorithm RSA `
        -KeyLength 3072 `
        -HashAlgorithm SHA256 `
        -KeyExportPolicy NonExportable `
        -NotAfter (Get-Date).AddYears($ValidYears)
    Write-Host "Created test certificate $($cert.Thumbprint)."
}

$outDirectory = Split-Path -Path $OutFile -Parent
if (-not (Test-Path -Path $outDirectory)) {
    New-Item -ItemType Directory -Path $outDirectory -Force | Out-Null
}
Export-Certificate -Cert $cert -FilePath $OutFile -Type CERT | Out-Null
$resolvedOut = (Resolve-Path -Path $OutFile).Path

Write-Host ''
Write-Host "Public certificate: $resolvedOut"
Write-Host "Thumbprint:         $($cert.Thumbprint)"
Write-Host ''
Write-Host 'Sign builds with it:'
Write-Host "  `$env:MWB_TEST_CERT_THUMBPRINT = '$($cert.Thumbprint)'"
Write-Host '  msbuild native\virtual-mic\virtual-mic.vcxproj /p:Configuration=Debug /p:Platform=x64'
Write-Host ''
Write-Host 'In the test VM (elevated PowerShell), copy the .cer over and run:'
Write-Host '  bcdedit /set testsigning on'
Write-Host '  Import-Certificate -FilePath .\MobileWebcamBridgeTest.cer -CertStoreLocation Cert:\LocalMachine\Root'
Write-Host '  Import-Certificate -FilePath .\MobileWebcamBridgeTest.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher'
Write-Host '  # then reboot'
