<#
.SYNOPSIS
  Hidden-desktop test for short-lived process capture (Phase 7 item 1): History entries and a ProcessRules rule.

.DESCRIPTION
  Never touches the user's settings or desktop: builds a scratch settings file + rules file, runs the app through
  tools\run-hidden.ps1 (private invisible desktop, job object), then checks the files the app wrote.

  Default mode (works without elevation): the plugin's self-test hook injects one synthetic short-lived process
  (tmp-selftest.exe) through the same code path the ETW events take. Checks:
    - tmp-history.bin contains the name "tmp-selftest.exe" (the capture reached History)
    - the rule for tmp-selftest.exe ran its command (marker file with the name and pid) (the capture reached Rules)

  -RealEtw (run from an ELEVATED PowerShell, because an ETW session needs administrator rights): no injection;
  instead a copy of hostname.exe named tmp-etwprobe.exe is started 25 times while the app runs. The same two checks
  must then hold for tmp-etwprobe.exe. Without elevation this mode only checks that the app logged the
  "needs administrator" message path by not crashing (it cannot read the log), so it refuses to run.

.EXAMPLE
  powershell -File tools\tests\capture-hidden.ps1
  powershell -File tools\tests\capture-hidden.ps1 -RealEtw     # elevated shell
#>
param([switch]$RealEtw)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runHidden = Join-Path $root "tools\run-hidden.ps1"

if ($RealEtw) {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "-RealEtw needs an elevated PowerShell (ETW sessions require administrator rights)."
    }
}

$dir = Join-Path $env:TEMP "tmplus-capture-test"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $dir | Out-Null

$name = if ($RealEtw) { "tmp-etwprobe.exe" } else { "tmp-selftest.exe" }
$marker = Join-Path $dir "rule-ran.txt"

$selfTest = if ($RealEtw) { "0" } else { "1" }
$settings = @{
    '$schema' = "settings.schema.json"
    "TaskManagerPlus.ProcessHistory.IntervalSeconds" = "1"
    "TaskManagerPlus.ProcessHistory.CaptureSelfTest" = $selfTest
    "TaskManagerPlus.ProcessHistory.CaptureShortLived" = "1"
}
$settings | ConvertTo-Json | ForEach-Object { [IO.File]::WriteAllText((Join-Path $dir "settings.json"), $_, (New-Object Text.UTF8Encoding($false))) }

$rules = @{
    version = 1
    rules = @(@{
        id = 1; enabled = $true; program = $name; path = ""
        startPriority = 0; startAffinity = 0; startAffinityMask = 0; startEfficiency = $false; startEnd = $false
        command = "cmd /c echo {name} {pid}>> $marker"
        watchdogs = @()
    })
}
$rules | ConvertTo-Json -Depth 6 -Compress | ForEach-Object { $_.Replace([string][char]92 + "u003e", ">") } | ForEach-Object { [IO.File]::WriteAllText((Join-Path $dir "tmp-rules.json"), $_, (New-Object Text.UTF8Encoding($false))) }

$probe = $null
if ($RealEtw) {
    $probe = Join-Path $dir $name
    Copy-Item (Join-Path $env:SystemRoot "System32\hostname.exe") $probe
    $job = Start-Job -ScriptBlock {
        param($p)
        Start-Sleep -Seconds 12
        1..25 | ForEach-Object { Start-Process -FilePath $p -WindowStyle Hidden -Wait; Start-Sleep -Milliseconds 300 }
    } -ArgumentList $probe
}

& $runHidden -Name capture -Settings (Join-Path $dir "settings.json") -Wait 24 -Wait2 2 -ExitCmd | Out-Null
if ($job) { Wait-Job $job -Timeout 60 | Out-Null; Remove-Job $job -Force }

$fail = 0
function Check($ok, $text) { if ($ok) { "PASS  $text" } else { "FAIL  $text"; $script:fail++ } }

$hist = Join-Path $dir "tmp-history.bin"
$histOk = $false
if (Test-Path $hist) {
    $bytes = [IO.File]::ReadAllBytes($hist)
    $needle = [Text.Encoding]::Unicode.GetBytes($name)
    $text = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
    $histOk = $text.Contains([Text.Encoding]::GetEncoding(28591).GetString($needle))
}
Check $histOk "History file contains $name"

$markerOk = (Test-Path $marker) -and ((Get-Content $marker -Raw) -match [regex]::Escape($name))
Check $markerOk "rule command ran for $name ($(if (Test-Path $marker) { (Get-Content $marker -Raw).Trim().Split("`n")[0] } else { 'no marker file' }))"

if ($fail) { "capture test FAILED ($fail)"; exit 1 }
"capture test passed"
