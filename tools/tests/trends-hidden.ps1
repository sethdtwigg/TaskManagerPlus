<#
.SYNOPSIS
  Hidden-desktop test for ResourceTrends (Phase 7 item 4): the on-disk trend file, the lookup ranking, the leak
  detector with a synthetic memory-growing process, persistence across a restart and screenshots of the Trends tab.
  Never touches the user's settings or desktop (tools\run-hidden.ps1, scratch settings and data files).

.DESCRIPTION
  Run 1 (dark theme, leak suspects view): starts a synthetic leaker (a PowerShell that allocates 2 MB per second) and
  a CPU spinner (a renamed copy of cmd.exe running an endless loop), then the app with 1 second samples, a 20 second
  leak window and a 20 MB growth threshold. Checks, from the plugin's test output file and the trend file:
    - tmp-trends.bin has the 'TMTR' header, version 1, samples and the spinner's name
    - the leak detector flagged the leaker's pid with at least 20 MB growth
    - the lookup (ranking over the whole recorded range) lists the spinner among the 5 busiest processes
  Run 2 (light theme, usage view) starts the app again on the same data and checks that the earlier samples were loaded
  (persistence); run 3 (OLED) is a screenshot only. Screenshots: $env:TEMP\tmplus-trends-test\shots.

.EXAMPLE
  powershell -File tools\tests\trends-hidden.ps1
#>
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runHidden = Join-Path $root "tools\run-hidden.ps1"
$dir = Join-Path $env:TEMP "tmplus-trends-test"
$shots = Join-Path $dir "shots"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $shots | Out-Null

$fail = 0
function Check($ok, $text) { if ($ok) { "PASS  $text" } else { "FAIL  $text"; $script:fail++ } }

$report = Join-Path $dir "report.txt"
$settingsPath = Join-Path $dir "settings.json"

function Write-Settings($themeMode, $viewIndex) {
    $h = @{
        '$schema' = "settings.schema.json"
        "EnableThemeSupport" = "1"; "ThemeMode" = "$themeMode"; "ReadableLookOffered" = "1"; "CalmLookOffered" = "1"
        "TaskManagerPlus.ResourceTrends.SampleSeconds" = "1"
        "TaskManagerPlus.ResourceTrends.LeakWindowSeconds" = "20"
        "TaskManagerPlus.ResourceTrends.LeakMinGrowthMB" = "20"
        "TaskManagerPlus.ResourceTrends.LeakMinGrowthPercent" = "0"
        "TaskManagerPlus.ResourceTrends.LeakNotify" = "1"
        "TaskManagerPlus.ResourceTrends.TestOutput" = $report
        "TaskManagerPlus.ResourceTrends.TestSelectTab" = "1"
        "TaskManagerPlus.ResourceTrends.ViewIndex" = "$viewIndex"
        "TaskManagerPlus.ResourceTrends.RangeIndex" = "0"
    }
    [IO.File]::WriteAllText($settingsPath, ($h | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
}

function Copy-Shots($out) {
    if ($out -match "run folder: (.+)") {
        Get-ChildItem $Matches[1].Trim() -Filter *.png | ForEach-Object { Copy-Item $_.FullName (Join-Path $shots $_.Name) -Force }
    }
}

# synthetic processes
$spinExe = Join-Path $dir "tmp-cpuspin.exe"
Copy-Item (Join-Path $env:SystemRoot "System32\cmd.exe") $spinExe
function Start-Synthetic {
    $leakCode = '$l = New-Object System.Collections.ArrayList; 1..80 | ForEach-Object { [void]$l.Add((New-Object byte[] (2MB))); Start-Sleep -Milliseconds 1000 }'
    $script:leaker = Start-Process -FilePath powershell.exe -ArgumentList "-NoProfile", "-WindowStyle", "Hidden", "-Command", $leakCode -WindowStyle Hidden -PassThru
    $script:spinner = Start-Process -FilePath $spinExe -ArgumentList '/c "for /l %i in (1,0,2) do @rem"' -WindowStyle Hidden -PassThru
}
function Stop-Synthetic {
    foreach ($p in @($script:leaker, $script:spinner)) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
}

try {
    # ---- run 1: dark, leak suspects view
    Write-Settings 2 1
    Start-Synthetic
    $out = & $runHidden -Name trends-leaks-dark -Settings $settingsPath -Wait 9 -Wait2 1 -Width 1200 -Height 800 -ExitCmd `
        -Steps @("wait:38", "shotmain:trends-leaks-dark") -StepClass x | Out-String
    Copy-Shots $out

    $bin = Join-Path $dir "tmp-trends.bin"
    Check (Test-Path $bin) "tmp-trends.bin was written next to the scratch settings"
    $bytes = [IO.File]::ReadAllBytes($bin)
    Check ($bytes.Length -gt 100 -and [Text.Encoding]::ASCII.GetString($bytes, 0, 4) -eq "TMTR" -and [BitConverter]::ToUInt32($bytes, 4) -eq 1) "trend file header is 'TMTR' version 1 ($($bytes.Length) bytes)"
    $latin1 = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
    $needle = [Text.Encoding]::GetEncoding(28591).GetString([Text.Encoding]::Unicode.GetBytes("tmp-cpuspin.exe"))
    Check $latin1.Contains($needle) "trend file contains the spinner's process name"

    # count type-2 records by walking the file
    $off = 16; $samples = 0
    while ($off + 8 -le $bytes.Length) {
        $type = [BitConverter]::ToUInt32($bytes, $off); $len = [BitConverter]::ToUInt32($bytes, $off + 4)
        if ($off + 8 + $len -gt $bytes.Length) { break }
        if ($type -eq 2) { $samples++ }
        $off += 8 + $len
    }
    Check ($samples -ge 25) "trend file holds $samples samples from the ~45 s run (1 s interval)"

    $text = if (Test-Path $report) { Get-Content $report -Raw } else { "" }
    $leakLine = ($text -split "`r?`n") | Where-Object { $_ -match "^suspect\|$($script:leaker.Id)\|" } | Select-Object -First 1
    Check ($null -ne $leakLine) "leak detector flagged the growing process (pid $($script:leaker.Id)): $leakLine"
    if ($leakLine) {
        $growth = [int64](($leakLine -split "\|")[3])
        Check ($growth -ge 20480) "reported growth is at least 20 MB ($growth KB)"
    }
    Check (($text -split "`r?`n" | Where-Object { $_ -match "^suspect\|$($script:spinner.Id)\|" }).Count -eq 0) "the CPU spinner (flat memory) was not flagged"
    $topCpu = ($text -split "`r?`n") | Where-Object { $_ -match "^topcpu\|" }
    Check (($topCpu | Where-Object { $_ -match "tmp-cpuspin.exe" }).Count -gt 0) "lookup ranks the spinner among the 5 busiest processes: $($topCpu -join ' ; ')"
    $firstSamples = [int](($text -split "`r?`n" | Where-Object { $_ -match "^samples=" }) -replace "samples=", "")
    Stop-Synthetic

    # ---- run 2: light, usage view; the file from run 1 must be loaded
    Write-Settings 1 0
    Remove-Item $report -ErrorAction SilentlyContinue
    $out = & $runHidden -Name trends-usage-light -Settings $settingsPath -Wait 9 -Wait2 1 -Width 1200 -Height 800 -ExitCmd -Steps @("wait:4", "shotmain:trends-usage-light") -StepClass x | Out-String
    Copy-Shots $out
    $text2 = if (Test-Path $report) { Get-Content $report -Raw } else { "" }
    $loaded = [int](($text2 -split "`r?`n" | Where-Object { $_ -match "^samples=" }) -replace "samples=", "")
    Check ($loaded -gt 0 -and $loaded -ge $samples - 2) "a restart loads the stored samples ($loaded samples in memory after start-up, file had $samples)"

    # ---- run 3: OLED, usage view
    Write-Settings 5 0
    $out = & $runHidden -Name trends-usage-oled -Settings $settingsPath -Wait 9 -Wait2 1 -Width 1200 -Height 800 -ExitCmd -Steps @("wait:3", "shotmain:trends-usage-oled") -StepClass x | Out-String
    Copy-Shots $out
    Check (Test-Path (Join-Path $shots "trends-usage-oled.png")) "OLED screenshot taken"
}
finally {
    Stop-Synthetic
    & $runHidden -Cleanup | Out-Null
}
"screenshots: $shots"
if ($fail) { "trends test FAILED ($fail)"; exit 1 }
"trends test passed"
