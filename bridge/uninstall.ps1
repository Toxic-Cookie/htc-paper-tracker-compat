<#
.SYNOPSIS
  Stops the VIVE Facial Tracker -> PaperTracker bridge and removes it and its logon task.
#>
[CmdletBinding()]
param([switch]$KeepLogs)
$ErrorActionPreference = 'Stop'

$dest = Join-Path $env:LOCALAPPDATA 'Programs\vft-papertracker-bridge'
$taskName = 'VFT PaperTracker Bridge'
$con = Join-Path $dest 'vft-papertracker-bridge.exe'

# Graceful stop first so the tracker's IR illuminator is switched off.
if (Test-Path $con) { & $con --stop | Out-Host }

if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    Write-Host "Removed logon task '$taskName'"
}
if (Test-Path $dest) {
    Remove-Item $dest -Recurse -Force
    Write-Host "Removed $dest"
}
$logs = Join-Path $env:LOCALAPPDATA 'vft-papertracker-bridge'
if (-not $KeepLogs -and (Test-Path $logs)) {
    Remove-Item $logs -Recurse -Force
    Write-Host "Removed $logs"
}
