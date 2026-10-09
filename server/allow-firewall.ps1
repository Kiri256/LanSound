#Requires -RunAsAdministrator
param(
    [Parameter(Mandatory = $true)]
    [string]$Server,
    [ValidateRange(1, 65535)]
    [int]$Port = 30010,
    [ValidateRange(1, 65535)]
    [int]$DiscoveryPort = 30011
)
$ErrorActionPreference = 'Stop'
$serverPath = (Resolve-Path -LiteralPath $Server).Path
if (!(Test-Path -LiteralPath $serverPath -PathType Leaf)) { throw 'Server must be an executable file.' }
if ($Port -eq $DiscoveryPort) { throw 'Audio and discovery ports must differ.' }

$ruleName = 'LanAudio-UDP-In'
$rule = Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue
if ($rule) {
    $rule | Set-NetFirewallRule -Enabled True -Direction Inbound -Action Allow -Profile Any
    $rule | Get-NetFirewallApplicationFilter | Set-NetFirewallApplicationFilter -Program $serverPath
    $rule | Get-NetFirewallPortFilter | Set-NetFirewallPortFilter -Protocol UDP -LocalPort $Port,$DiscoveryPort -RemotePort Any
    $rule | Get-NetFirewallAddressFilter | Set-NetFirewallAddressFilter -LocalAddress Any -RemoteAddress LocalSubnet
} else {
    New-NetFirewallRule -Name $ruleName -DisplayName 'LanAudio UDP (local subnet)' `
        -Direction Inbound -Action Allow -Enabled True -Profile Any `
        -Program $serverPath -Protocol UDP -LocalPort $Port,$DiscoveryPort `
        -RemoteAddress LocalSubnet | Out-Null
}
Write-Output "Allowed UDP $Port/$DiscoveryPort on public/private/domain profiles, from the local subnet only: $serverPath"
