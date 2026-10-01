<#
.SYNOPSIS
  Exposes the host's phone service to a Hyper-V VM: usbmuxd (iPhone, 127.0.0.1:27015) or the ADB
  server (Android, 127.0.0.1:5037).

.DESCRIPTION
  Hyper-V cannot pass a phone's USB connection through to a VM. For driver testing inside a
  test-signing VM, forward the host's service port onto the host's Hyper-V virtual switch address
  so the bridge inside the VM can reach it. In the VM's bridge.config.json set
  "usbmux": { "address": "<HostIp>:27015" } (iPhone) or "adb": { "address": "<HostIp>:5037" } (Android).

  SECURITY: anyone who can reach the forwarded port can talk to your phone. Bind only
  to the internal/default switch address, restrict the firewall rule to the VM subnet, and run
  with -Remove when you are done. Requires an elevated PowerShell.

.EXAMPLE
  .\vm-portproxy.ps1 -HostIp 172.25.80.1 -VmSubnet 172.25.80.0/20
  .\vm-portproxy.ps1 -HostIp 172.25.80.1 -Remove
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $HostIp,
    [string] $VmSubnet,
    [int] $Port = 27015,
    [switch] $Remove
)

$ErrorActionPreference = 'Stop'
$ruleName = "Mobile Webcam Bridge port $Port for Hyper-V ($HostIp)"

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated PowerShell.'
}

if ($Remove) {
    netsh interface portproxy delete v4tov4 listenaddress=$HostIp listenport=$Port | Out-Null
    Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    Write-Host "Removed port proxy and firewall rule for $HostIp`:$Port"
    return
}

if (-not $VmSubnet) { throw 'Specify -VmSubnet (e.g. 172.25.80.0/20) so the firewall rule stays narrow.' }

netsh interface portproxy add v4tov4 listenaddress=$HostIp listenport=$Port connectaddress=127.0.0.1 connectport=$Port | Out-Null
if (-not (Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName $ruleName -Direction Inbound -Protocol TCP -LocalAddress $HostIp `
        -LocalPort $Port -RemoteAddress $VmSubnet -Action Allow | Out-Null
}
Write-Host "Port $Port is reachable from $VmSubnet at $HostIp`:$Port. Remove with -Remove when finished."
