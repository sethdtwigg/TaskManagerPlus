<#
.SYNOPSIS
  Hidden-desktop screenshot of the Network tab with the NetworkLookup columns (Remote service, Remote country).
  Uses a scratch settings file that opens on the Network tab; never touches the user's settings or desktop.
  Look at the png: established connections to port 443 should show "https" and a country name.
#>
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$dir = Join-Path $env:TEMP "tmplus-net-test"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$json = '{"$schema":"settings.schema.json","MainWindowTabRestoreEnabled":"1","MainWindowTabRestoreName":"Network","MainWindowTabRestoreIndex":"4"}'
[IO.File]::WriteAllText((Join-Path $dir "settings.json"), $json, (New-Object Text.UTF8Encoding($false)))
& (Join-Path $root "tools\run-hidden.ps1") -Name network -Settings (Join-Path $dir "settings.json") -Wait 10 -Wait2 4 -Width 1700 -Height 500
