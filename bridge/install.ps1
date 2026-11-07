<#
.SYNOPSIS
  Installs the VIVE Facial Tracker -> PaperTracker bridge for the current user and starts it at logon.
.DESCRIPTION
  Copies the bridge to %LOCALAPPDATA%\Programs\vft-papertracker-bridge and registers a per-user
  scheduled task "VFT PaperTracker Bridge" that runs the windowless bridge at logon. The bridge idles
  until PaperTracker is running, then activates the tracker; it switches the tracker off again when
  PaperTracker closes. No administrator rights required. Re-run to update; settings are kept.
#>
[CmdletBinding()]
param([switch]$NoStart)
$ErrorActionPreference = 'Stop'

$here = $PSScriptRoot
$dest = Join-Path $env:LOCALAPPDATA 'Programs\vft-papertracker-bridge'
$taskName = 'VFT PaperTracker Bridge'
$con = 'vft-papertracker-bridge.exe'
$bg = 'vft-papertracker-bridge-bg.exe'
$cfg = 'vft-papertracker-bridge.cfg'

if (-not (Test-Path (Join-Path $here "out\$bg"))) {
    Write-Host 'Building...'
    & cmd /c "`"$here\build.cmd`""
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
}

# Stop a running copy so its files can be replaced (it switches the tracker off first).
$existing = Join-Path $dest $con
if (Test-Path $existing) { & $existing --stop | Out-Host }

New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item (Join-Path $here "out\$con"), (Join-Path $here "out\$bg"), (Join-Path $here 'README.md') $dest -Force
if (-not (Test-Path (Join-Path $dest $cfg))) {
    Copy-Item (Join-Path $here $cfg) $dest
} else {
    Write-Host "Keeping existing settings: $(Join-Path $dest $cfg)"
}

$user = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
$action = New-ScheduledTaskAction -Execute (Join-Path $dest $bg) -WorkingDirectory $dest
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew `
    -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Settings $settings `
    -Principal $principal -Description 'Streams the HTC VIVE Facial Tracker into PaperTracker (no SRanipal).' `
    -Force | Out-Null

if (-not $NoStart) { Start-ScheduledTask -TaskName $taskName }

Write-Host ''
Write-Host "Installed to $dest"
Write-Host "Logon task  : $taskName$(if (-not $NoStart) { ' (started)' })"
Write-Host "Settings    : $(Join-Path $dest $cfg)"
Write-Host "Log         : $(Join-Path $env:LOCALAPPDATA 'vft-papertracker-bridge\bridge.log')"
