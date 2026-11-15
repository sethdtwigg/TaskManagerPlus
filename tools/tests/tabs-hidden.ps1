<#
.SYNOPSIS
  Hidden-desktop regression test for the main-window tab strip: "More" menu, tab/page index bookkeeping and the default
  tab set (docs\ui-declutter-plan.md, section 12). Never touches the user's settings or desktop (tools\run-hidden.ps1).

.DESCRIPTION
  Bug (2026-11-14): after choosing an entry of the "More" menu the previously shown page was never hidden (the "old"
  index was the More tab), so it kept covering the pages selected later: clicking Processes showed the Startup list.

  Run 1 (scratch settings, hidden list = Information,Disk,Firewall,Devices,History,Rules,Trends,Advisor): from three
  starting tabs, every More entry is chosen through the real popup menu (More tab, Down x n, Return) and then every
  strip tab is selected. After each step exactly ONE page window may be visible, the selected tab must be the clicked one,
  and each tab must always show the same window (and two tabs never the same one).
  Run 2 (restart with MainWindowTabRestoreName = Devices, a hidden tab): the restored tab is shown, then the strip tabs
  are walked again.
  Runs 3-5 check the default set from the settings file the app writes on exit: a fresh install hides everything but
  Processes, Performance, Services, Network, Startup; an old default list (without Trends, Advisor) is upgraded once; a
  user's own list is not touched.

.EXAMPLE
  powershell -File tools\tests\tabs-hidden.ps1
#>
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runHidden = Join-Path $root "tools\run-hidden.ps1"
$dir = Join-Path $env:TEMP "tmplus-tabs-test"
$shots = Join-Path $dir "shots"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $shots | Out-Null

$fail = 0
function Check($ok, $text) { if ($ok) { "PASS  $text" } else { "FAIL  $text"; $script:fail++ } }

$settings = Join-Path $dir "settings.json"
function Write-Settings($h) {
    $h['$schema'] = "settings.schema.json"; $h["EnableThemeSupport"] = "1"; $h["ReadableLookOffered"] = "1"
    [IO.File]::WriteAllText($settings, ($h | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
}
function Read-Setting($name) { $j = Get-Content $settings -Raw | ConvertFrom-Json; return [string]$j.$name }
function Run($name, $steps) {
    $o = & $runHidden -Name $name -Settings $settings -Wait 10 -Wait2 1 -Width 1400 -Height 800 -Steps ($steps -join ";") -ExitCmd | Out-String
    if ($o -match "run folder: (.+)") { Get-ChildItem $Matches[1].Trim() -Filter *.png | ForEach-Object { Copy-Item $_.FullName (Join-Path $shots $_.Name) -Force } }
    return $o
}

$visible = "Processes", "Performance", "Services", "Network", "Startup"
$moreEntries = "Information", "Disk", "Firewall", "Devices", "History", "Rules", "Trends", "Advisor"   # popup order = strip order
$newDefault = "Information,Disk,Firewall,Devices,History,Rules,Trends,Advisor"
$oldDefault = "Information,Disk,Firewall,Devices,History,Rules"

& $runHidden -Cleanup | Out-Null
if (Get-Process -Name SystemInformer -ErrorAction SilentlyContinue) {
    "NOTE  a SystemInformer.exe is running (the elevated instance blocks the private namespace for a copy started without elevation); close it first"
}

try {
    # ---- run 1: every More entry from several starting tabs, then every strip tab
    Write-Settings @{ "CalmLookOffered" = "1"; "MainWindowHiddenTabsMigrated" = "1"; "MainWindowHiddenTabs" = $newDefault }
    $s = @("tabs", "tabcheck:Processes")
    foreach ($start in "Startup", "Processes") {
        for ($i = 0; $i -lt $moreEntries.Count; $i++) {
            $e = $moreEntries[$i]
            $s += "tabgo:$start"; $s += "tabcheck:$start"
            $s += "tabmore:$e,$($i + 1)"; $s += "tabcheck:$e"
            $walk = if ($start -eq "Startup" -and $i -lt 2) { $visible } else { "Processes", "Startup" }
            foreach ($v in $walk) { $s += "tabgo:$v"; $s += "tabcheck:$v" }
        }
    }
    # More twice in a row (second pick replaces the temporary tab; the revealed Disk is still listed, so Rules is 6th), then back and forth
    $s += "tabgo:Startup"; $s += "tabmore:Disk,2"; $s += "tabcheck:Disk"; $s += "tabmore:Rules,6"; $s += "tabcheck:Rules"
    foreach ($v in $visible) { $s += "tabgo:$v"; $s += "tabcheck:$v" }
    $o = Run "tabs-run1" $s
    ($o -split "`r?`n" | Where-Object { $_ -match "^(FAIL|tab strip|tabcheck failures)" }) | Select-Object -First 12
    $pass = ([regex]::Matches($o, "(?m)^PASS ")).Count
    Check ($o -match "tabcheck failures: 0") "run 1: no tab/page mismatch ($pass checks passed)"
    Check ($o -match "tab strip: 0=Processes 1=Performance") "run 1: strip read back"

    # ---- run 2: restart with a hidden tab remembered
    Write-Settings @{ "CalmLookOffered" = "1"; "MainWindowHiddenTabsMigrated" = "1"; "MainWindowHiddenTabs" = $newDefault; "MainWindowTabRestoreEnabled" = "1"; "MainWindowTabRestoreName" = "Devices"; "MainWindowTabRestoreIndex" = "7" }
    $s = @("tabcheck:Devices")
    foreach ($v in $visible) { $s += "tabgo:$v"; $s += "tabcheck:$v" }
    $s += "tabmore:Disk,2"; $s += "tabcheck:Disk"; $s += "tabgo:Processes"; $s += "tabcheck:Processes"
    $o = Run "tabs-run2" $s
    ($o -split "`r?`n" | Where-Object { $_ -match "^FAIL" }) | Select-Object -First 8
    Check ($o -match "tabcheck failures: 0") "run 2: restored hidden tab shows its page, then every strip tab matches"

    # ---- run 3: fresh install default (settings file without any look/tab keys)
    Write-Settings @{}
    $o = Run "tabs-run3" @("tabs")
    Check ((Read-Setting "MainWindowHiddenTabs") -eq $newDefault) "run 3: fresh install keeps Information,Disk,Firewall,Devices,History,Rules,Trends,Advisor under More (strip: Processes, Performance, Services, Network, Startup)"

    # ---- run 4: old default list is upgraded once
    Write-Settings @{ "CalmLookOffered" = "1"; "MainWindowHiddenTabs" = $oldDefault }
    $o = Run "tabs-run4" @("tabs")
    Check ((Read-Setting "MainWindowHiddenTabs") -eq $newDefault) "run 4: old default list gets Trends and Advisor under More"

    # ---- run 5: the user's own choices are not overwritten (custom list; and "show everything" = empty list)
    Write-Settings @{ "CalmLookOffered" = "1"; "MainWindowHiddenTabs" = "Disk,Rules" }
    $o = Run "tabs-run5a" @("tabs")
    Check ((Read-Setting "MainWindowHiddenTabs") -eq "Disk,Rules") "run 5: a custom hidden list is kept"
    Write-Settings @{ "CalmLookOffered" = "1"; "MainWindowHiddenTabs" = "" }
    $o = Run "tabs-run5b" @("tabs")
    Check ((Read-Setting "MainWindowHiddenTabs") -eq "") "run 5: an empty list (all tabs shown) is kept"
}
finally {
    & $runHidden -Cleanup | Out-Null
}

"screenshots: $shots"
if ($fail) { "tabs-hidden: $fail FAILED"; exit 1 } else { "tabs-hidden: all passed" }
