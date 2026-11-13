<#
.SYNOPSIS
  Hidden-desktop test for boot timing in the Startup tab (Phase 7 item 7). Never touches the user's settings or desktop.

.DESCRIPTION
  Run A uses the real local event log (Microsoft-Windows-Diagnostics-Performance/Operational) without sample data:
  the Boot timing window must open and say something sensible whether the log is readable, empty or access is denied
  (screenshot boot-real-window.png), and the Startup export still works (graceful degradation).
  Runs B (dark), C (light), D (OLED) set TaskManagerPlus.StartupManager.BootSampleFile to a generated file of
  Diagnostics-Performance events (a boot record, a slow startup app that matches a real entry of this PC's startup list,
  a service, a driver) and check, from a startup export (SessionReports, JSON):
    - the matching entry has Boot delay 7000 ms and Impact "High", others keep their estimate
  plus screenshots of the Startup tab and of the Boot timing window in each theme.
  The unit tests (tools\tests\unit-tests.ps1, test-boot.c) cover the parser and matching with sample XML.

.EXAMPLE
  powershell -File tools\tests\boot-hidden.ps1
#>
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runHidden = Join-Path $root "tools\run-hidden.ps1"
$dir = Join-Path $env:TEMP "tmplus-boot-test"
$shots = Join-Path $dir "shots"
$out = Join-Path $dir "out"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $shots, $out | Out-Null

$fail = 0
function Check($ok, $text) { if ($ok) { "PASS  $text" } else { "FAIL  $text"; $script:fail++ } }

function Write-Settings($name, $themeMode, $sample) {
    $h = @{
        '$schema' = "settings.schema.json"; "EnableThemeSupport" = "1"; "ThemeMode" = "$themeMode"; "ReadableLookOffered" = "1"; "CalmLookOffered" = "1"
        "TaskManagerPlus.SessionReports.TestDirectory" = $out
    }
    if ($sample) { $h["TaskManagerPlus.StartupManager.BootSampleFile"] = $sample }
    $path = Join-Path $dir "$name.json"
    [IO.File]::WriteAllText($path, ($h | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
    return $path
}

function Invoke-Run($name, $settings, $steps) {
    $o = & $runHidden -Name $name -Settings $settings -Wait 9 -Wait2 1 -Width 1300 -Height 800 -Steps $steps | Out-String
    if ($o -match "run folder: (.+)") {
        Get-ChildItem $Matches[1].Trim() -Filter *.png | ForEach-Object { Copy-Item $_.FullName (Join-Path $shots $_.Name) -Force }
        $d = Join-Path $Matches[1].Trim() "step-dialog.png"
        if (Test-Path $d) { Copy-Item $d (Join-Path $shots "$name-window.png") -Force }
    }
    return $o
}

$export = "msg:TaskManagerPlus.SessionReports.Export,1,1"   # startup list as JSON
$select = "msg:TaskManagerPlus.StartupManager.Command,1"
$window = "msg:TaskManagerPlus.StartupManager.Command,2"

try {
    # ---- run A: real log, graceful whatever it holds
    $real = $null
    try { $real = Get-WinEvent -LogName "Microsoft-Windows-Diagnostics-Performance/Operational" -MaxEvents 1 -ErrorAction Stop } catch { $real = $_.Exception.Message }
    "   real log readable from this shell: $(if ($real -is [string]) { 'no (' + $real.Trim() + ')' } elseif ($real) { 'yes' } else { 'empty' })"
    $s = Write-Settings "real" 2 $null
    $o = Invoke-Run "boot-real" $s @($select, "wait:2", "shotmain:real-startup-tab", $export, "wait:1", $window, "wait:2", "dialog:yes", "wait:1")
    Check ($o -match "dialog answered yes") "Boot timing window opened and closed with the real event log (status shown, no crash)"
    Check (Test-Path (Join-Path $out "report-startup.json")) "startup export works with the real log ($(if ($real -is [string]) { 'access denied or unreadable' } else { 'readable' }))"
    $rows = (Get-Content (Join-Path $out "report-startup.json") -Raw -Encoding UTF8 | ConvertFrom-Json).rows
    Check (@($rows).Count -gt 0) "startup list has $(@($rows).Count) entries"
    $target = @($rows | Where-Object { $_.Path -and (Test-Path -LiteralPath $_.Path) -and $_.Path -match '\.exe$' } | Select-Object -First 1)
    if (-not $target.Count) { throw "no startup entry with an existing .exe path on this machine to match against" }
    $exeName = [IO.Path]::GetFileName($target[0].Path)
    "   matching entry: $($target[0].Name) -> $exeName (estimate: $($target[0].Impact))"

    # ---- sample events file
    $now = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.0000000Z")
    $ev = @(
        "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><EventID>100</EventID><TimeCreated SystemTime='$now'/></System><EventData><Data Name='BootTime'>65432</Data><Data Name='MainPathBootTime'>41000</Data><Data Name='BootPostBootTime'>24432</Data><Data Name='BootDriverInitTime'>3100</Data><Data Name='BootSmssInitTime'>7400</Data><Data Name='BootExplorerInitTime'>6200</Data><Data Name='BootNumStartupApps'>9</Data><Data Name='BootIsDegradation'>1</Data></EventData></Event>",
        "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><EventID>101</EventID><TimeCreated SystemTime='$now'/></System><EventData><Data Name='Name'>$exeName</Data><Data Name='FriendlyName'>Sample slow app</Data><Data Name='Path'>C:\Elsewhere\$exeName</Data><Data Name='TotalTime'>7000</Data><Data Name='DegradationTime'>4800</Data></EventData></Event>",
        "<Event><System><EventID>103</EventID><TimeCreated SystemTime='$now'/></System><EventData><Data Name='Name'>SampleSlowService</Data><Data Name='TotalTime'>2200</Data><Data Name='DegradationTime'>900</Data></EventData></Event>",
        "<Event><System><EventID>102</EventID><TimeCreated SystemTime='$now'/></System><EventData><Data Name='Name'>\SystemRoot\system32\drivers\sample.sys</Data><Data Name='TotalTime'>1500</Data></EventData></Event>"
    )
    $sample = Join-Path $dir "sample-events.xml"
    [IO.File]::WriteAllText($sample, ($ev -join "`n"), (New-Object Text.UTF8Encoding($false)))

    # ---- runs B, C, D: sample data in three themes
    foreach ($t in @(@("dark", 2), @("light", 1), @("oled", 5))) {
        Remove-Item (Join-Path $out "report-startup.json") -ErrorAction SilentlyContinue
        $s = Write-Settings "sample-$($t[0])" $t[1] $sample
        $o = Invoke-Run "boot-$($t[0])" $s @($select, "wait:2", "shotmain:startup-tab-$($t[0])", $export, "wait:1", $window, "wait:2", "dialog:yes", "wait:1")
        Check ($o -match "dialog answered yes") "Boot timing window opened ($($t[0]) theme)"
        $jp = Join-Path $out "report-startup.json"
        if (-not (Test-Path $jp)) { Check $false "startup export ($($t[0]))"; continue }
        $rows = (Get-Content $jp -Raw -Encoding UTF8 | ConvertFrom-Json).rows
        $hit = @($rows | Where-Object { $_.Path -eq $target[0].Path -and $_.Name -eq $target[0].Name })
        Check ($hit.Count -ge 1 -and $hit[0].'Boot delay (ms)' -eq "7000" -and $hit[0].Impact -eq "High") "matching entry got Boot delay 7000 ms and Impact High ($($t[0]) theme; was $($target[0].Impact))"
        $others = @($rows | Where-Object { $_.'Boot delay (ms)' -eq "" -or $null -eq $_.'Boot delay (ms)' })
        Check ($others.Count -gt 0 -or @($rows).Count -eq $hit.Count) "entries without log data keep no boot delay ($($others.Count) of $(@($rows).Count))"
    }
}
finally {
    & $runHidden -Cleanup | Out-Null
}

"screenshots: $shots"
if ($fail) { "boot timing test FAILED ($fail)"; exit 1 }
"boot timing test passed"
