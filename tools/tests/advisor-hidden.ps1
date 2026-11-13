<#
.SYNOPSIS
  Hidden-desktop test for the Efficiency advisor (Phase 7 item 5). Never touches the user's settings or desktop
  (tools\run-hidden.ps1, scratch settings).

.DESCRIPTION
  Starts throwaway CPU spinners (copies of cmd.exe running an endless loop, so they have unmistakable names) and runs the
  app with short advisor thresholds (5 s in the background, 1 % CPU). Checks, from the plugin's test output file:
    - the spinner is ranked among the top offenders with a CPU reason
    - none of System, Registry, csrss.exe, svchost.exe, dwm.exe, explorer.exe, lsass.exe or System Informer itself is listed
  Then the one-click actions go through the same handlers as the buttons (EnableWarnings = 0 so the core confirmation does
  not wait for a click on the hidden desktop):
    - Efficiency mode on spinner A: the process ends up with Idle priority class (what Eco mode sets)
    - Lower priority on spinner B: BelowNormal
    - Ignore on spinner C: the program is in the ignore list setting and gone from the next ranking
  and nothing at all happens to spinner D, which is only listed (the advisor never acts without a click).
  Screenshots of the Advisor tab in dark, light and OLED themes.
  The foreground/background logic is covered by unit tests (test-advisor.c): the hidden desktop has no real foreground app (TestAllowNoForeground keeps the advisor sampling there).

.EXAMPLE
  powershell -File tools\tests\advisor-hidden.ps1
#>
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runHidden = Join-Path $root "tools\run-hidden.ps1"
$dir = Join-Path $env:TEMP "tmplus-advisor-test"
$shots = Join-Path $dir "shots"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $shots | Out-Null

$fail = 0
function Check($ok, $text) { if ($ok) { "PASS  $text" } else { "FAIL  $text"; $script:fail++ } }

$settings = Join-Path $dir "settings.json"
$report = Join-Path $dir "advisor.txt"
$cmd = "msg:TaskManagerPlus.EfficiencyAdvisor.Command"

function Write-Settings($theme, $program) {
    $h = @{
        '$schema' = "settings.schema.json"; "EnableThemeSupport" = "1"; "ThemeMode" = "$theme"; "ReadableLookOffered" = "1"; "CalmLookOffered" = "1"
        "EnableWarnings" = "0"
        "TaskManagerPlus.EfficiencyAdvisor.MinBackgroundSeconds" = "5"
        "TaskManagerPlus.EfficiencyAdvisor.HalfLifeSeconds" = "20"
        "TaskManagerPlus.EfficiencyAdvisor.CpuThresholdTenthsPercent" = "10"
        "TaskManagerPlus.EfficiencyAdvisor.TestAllowNoForeground" = "1"
        "TaskManagerPlus.EfficiencyAdvisor.TestOutput" = $report
        "TaskManagerPlus.EfficiencyAdvisor.TestProgram" = $program
    }
    if (Test-Path $settings) {
        $old = Get-Content $settings -Raw | ConvertFrom-Json
        if ($old.'TaskManagerPlus.EfficiencyAdvisor.IgnoredPrograms') { $h["TaskManagerPlus.EfficiencyAdvisor.IgnoredPrograms"] = $old.'TaskManagerPlus.EfficiencyAdvisor.IgnoredPrograms' }
    }
    [IO.File]::WriteAllText($settings, ($h | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
}

function Start-Spinner($name) {
    $exe = Join-Path $dir "$name.exe"
    Copy-Item (Join-Path $env:SystemRoot "System32\cmd.exe") $exe
    Start-Process -FilePath $exe -ArgumentList '/c "for /l %i in (1,0,2) do @rem"' -WindowStyle Hidden -PassThru
}

function Run($name, $theme, $program, $steps) {
    Write-Settings $theme $program
    $o = & $runHidden -Name $name -Settings $settings -Wait 9 -Wait2 1 -Width 1400 -Height 800 -Steps $steps -ExitCmd | Out-String
    if ($o -match "run folder: (.+)") { Get-ChildItem $Matches[1].Trim() -Filter *.png | ForEach-Object { Copy-Item $_.FullName (Join-Path $shots $_.Name) -Force } }
    return $o
}

$procs = @{}
try {
    foreach ($n in "tmp-adv-a", "tmp-adv-b", "tmp-adv-c", "tmp-adv-d") { $procs[$n] = Start-Spinner $n }

    # ---- run 1: ranking, exclusions, Efficiency mode on A
    $o = Run "advisor-dark" 2 "tmp-adv-a.exe" @("$cmd,1", "wait:22", "$cmd,5", "wait:1", "shotmain:advisor-dark", "$cmd,2", "wait:2", "$cmd,5", "wait:2")
    Check (Test-Path $report) "advisor test output written"
    $lines = @(Get-Content $report -Encoding UTF8)
    $names = @($lines | ForEach-Object { ($_ -split "\|")[0] })
    "   ranking: $($names[0..([Math]::Min(7, $names.Count - 1))] -join ', ')"
    $rank = [Array]::IndexOf($names, "tmp-adv-a.exe")
    Check ($rank -ge 0 -and $rank -lt 8) "spinner tmp-adv-a.exe is among the top offenders (position $($rank + 1) of $($names.Count))"
    Check (($lines | Where-Object { $_ -like "tmp-adv-a.exe|*|*|CPU:*" }).Count -eq 1) "its reason is CPU"
    foreach ($bad in "System", "Registry", "csrss.exe", "svchost.exe", "dwm.exe", "explorer.exe", "lsass.exe", "SystemInformer.exe", "MsMpEng.exe", "services.exe") {
        Check ($names -notcontains $bad) "never listed: $bad"
    }
    $procs["tmp-adv-a"].Refresh()
    Check ((Get-Process -Id $procs["tmp-adv-a"].Id).PriorityClass -eq "Idle") "Efficiency mode on tmp-adv-a.exe (Idle priority class set by the real handler)"
    Check ((Get-Process -Id $procs["tmp-adv-d"].Id).PriorityClass -eq "Normal") "tmp-adv-d.exe is only listed, untouched (priority still Normal)"
    Check ((Get-Process -Id $procs["tmp-adv-b"].Id).PriorityClass -eq "Normal") "tmp-adv-b.exe untouched before its action"

    # ---- run 2: lower priority on B
    $o = Run "advisor-lower" 1 "tmp-adv-b.exe" @("$cmd,1", "wait:22", "$cmd,3", "wait:2")
    Check ((Get-Process -Id $procs["tmp-adv-b"].Id).PriorityClass -eq "BelowNormal") "Lower priority on tmp-adv-b.exe (BelowNormal)"
    Check ((Get-Process -Id $procs["tmp-adv-d"].Id).PriorityClass -eq "Normal") "tmp-adv-d.exe still untouched"

    # ---- run 3: ignore C, then it disappears from the ranking
    $o = Run "advisor-ignore" 5 "tmp-adv-c.exe" @("$cmd,1", "wait:22", "$cmd,5", "wait:1", "$cmd,4", "wait:2", "$cmd,5", "wait:2", "shotmain:advisor-oled")
    $s = Get-Content $settings -Raw | ConvertFrom-Json
    Check (($s.'TaskManagerPlus.EfficiencyAdvisor.IgnoredPrograms') -like "*tmp-adv-c.exe*") "Ignore stored tmp-adv-c.exe in the ignore list setting"
    $names = @(Get-Content $report -Encoding UTF8 | ForEach-Object { ($_ -split "\|")[0] })
    Check ($names -notcontains "tmp-adv-c.exe" -and $names -contains "tmp-adv-d.exe") "ignored program is gone from the ranking, the others stay"

    # ---- screenshot in the light theme
    $o = Run "advisor-light" 1 "" @("$cmd,1", "wait:22", "$cmd,5", "wait:1", "shotmain:advisor-light")
    Check ((Test-Path (Join-Path $shots "advisor-light.png")) -and (Test-Path (Join-Path $shots "advisor-dark.png")) -and (Test-Path (Join-Path $shots "advisor-oled.png"))) "screenshots of the Advisor tab in dark, light and OLED themes"
}
finally {
    foreach ($p in $procs.Values) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
    & $runHidden -Cleanup | Out-Null
}

"screenshots: $shots"
if ($fail) { "advisor test FAILED ($fail)"; exit 1 }
"advisor test passed"
