<#
.SYNOPSIS
  Hidden-desktop test for Profiles (Phase 7 item 8): applying a profile changes the rules file and the theme setting,
  revert restores both, export/import of JSON, saving the current state as a profile, and screenshots of the Manage and
  Save dialogs and of the palette's profile commands. Never touches the user's settings or desktop (run-hidden.ps1,
  scratch settings, scratch tmp-rules.json / tmp-profiles.json next to them).

.DESCRIPTION
  Seeds: three rules (tmp-p1.exe on, below normal; tmp-p2.exe off, no priority; tmp-p3.exe on, idle), a "Gaming" profile
  (p1 off, p2 on with high priority, OLED theme; a rule that does not exist is named too) and the dark theme.
  Run 1 applies Gaming:  rules file p1 off, p2 on + high; settings ThemeMode 5; profiles file keeps the previous state.
  Run 2 reverts:         rules and theme back to the seeds; the previous snapshot is gone.
  Run 3 saves the current state as "Saved1" through the test seam, exports everything, then imports a file holding one
         good and two bad profiles (empty name, bad theme name).
  Runs 4-6 take screenshots (dark, light, OLED) of the Manage dialog; the Save dialog and the palette once.
  The profile logic (plans, only-differences, revert exactness, limits) is covered by test-profiles.c.

.EXAMPLE
  powershell -File tools\tests\profiles-hidden.ps1
#>
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runHidden = Join-Path $root "tools\run-hidden.ps1"
$dir = Join-Path $env:TEMP "tmplus-profiles-test"
$shots = Join-Path $dir "shots"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $shots | Out-Null

$fail = 0
function Check($ok, $text) { if ($ok) { "PASS  $text" } else { "FAIL  $text"; $script:fail++ } }
function Write-Json($path, $obj) { [IO.File]::WriteAllText($path, ($obj | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false))) }

$settings = Join-Path $dir "settings.json"
$rulesFile = Join-Path $dir "tmp-rules.json"
$profilesFile = Join-Path $dir "tmp-profiles.json"
$exportFile = Join-Path $dir "export.json"
$importFile = Join-Path $dir "import.json"

function Write-Settings($theme, $name) {
    Write-Json $settings @{
        '$schema' = "settings.schema.json"; "EnableThemeSupport" = "1"; "ThemeMode" = "$theme"; "ReadableLookOffered" = "1"; "CalmLookOffered" = "1"
        "TaskManagerPlus.Profiles.TestName" = $name
        "TaskManagerPlus.Profiles.TestFile" = $script:currentFile
    }
}

function Rule($program, $enabled, $priority) {
    @{ id = 0; enabled = $enabled; muted = $false; program = $program; path = ""; startPriority = $priority; startAffinity = 0; startAffinityMask = 0; startEfficiency = $false; startEnd = $false; command = ""
       watchdogs = @(1..4 | ForEach-Object { @{ enabled = $false; metric = 0; threshold = 0; duration = 0; action = 0 } }) }
}
function Seed {
    Write-Json $rulesFile @{ version = 1; rules = @((Rule "tmp-p1.exe" $true 5), (Rule "tmp-p2.exe" $false 0), (Rule "tmp-p3.exe" $true 1)) }
    Write-Json $profilesFile @{ version = 1; active = ""; profiles = @(
        @{ name = "Gaming"; theme = "oled"; rules = @(
            @{ program = "tmp-p1.exe"; enabled = "off" },
            @{ program = "TMP-P2.EXE"; enabled = "on"; priority = "high" },
            @{ program = "tmp-nothere.exe"; enabled = "off" }) },
        @{ name = "Work"; theme = "light"; rules = @(@{ program = "tmp-p3.exe"; enabled = "off"; priority = "none" }) }) }
}

function Run($name, $theme, $profile, $steps) {
    Write-Settings $theme $profile
    $o = & $runHidden -Name $name -Settings $settings -Wait 9 -Wait2 1 -Width 1100 -Height 800 -Steps $steps -ExitCmd | Out-String
    if ($o -match "run folder: (.+)") {
        Get-ChildItem $Matches[1].Trim() -Filter *.png | ForEach-Object { Copy-Item $_.FullName (Join-Path $shots $_.Name) -Force }
        $d = Join-Path $Matches[1].Trim() "step-dialog.png"
        if (Test-Path $d) { Copy-Item $d (Join-Path $shots "$name-dialog.png") -Force }
    }
    return $o
}
function Rules { (Get-Content $rulesFile -Raw -Encoding UTF8 | ConvertFrom-Json).rules }
function Theme { [Convert]::ToInt32((Get-Content $settings -Raw | ConvertFrom-Json).ThemeMode, 16) }
$cmd = "msg:TaskManagerPlus.Profiles.Command"
$script:currentFile = $exportFile

try {
    Seed
    # ---- run 1: apply Gaming
    $o = Run "profiles-apply" 2 "Gaming" @("wait:3", "$cmd,2", "wait:2", "shotmain:after-gaming")
    $r = Rules
    $p1 = $r | Where-Object { $_.program -eq "tmp-p1.exe" }; $p2 = $r | Where-Object { $_.program -eq "tmp-p2.exe" }; $p3 = $r | Where-Object { $_.program -eq "tmp-p3.exe" }
    Check (-not $p1.enabled -and $p1.startPriority -eq 5) "Gaming: p1 switched off, priority untouched (keep)"
    Check ($p2.enabled -and $p2.startPriority -eq 3) "Gaming: p2 switched on with high priority (program name matched case-insensitively)"
    Check ($p3.enabled -and $p3.startPriority -eq 1) "Gaming: p3 untouched"
    Check ((Theme) -eq 5) "Gaming: theme setting is now OLED (5)"
    $pf = Get-Content $profilesFile -Raw -Encoding UTF8 | ConvertFrom-Json
    Check ($pf.active -eq "Gaming") "profiles file records the active profile"
    Check ($null -ne $pf.previous -and $pf.previous.theme -eq "dark") "profiles file keeps the previous state (theme dark)"
    $pp1 = $pf.previous.rules | Where-Object { $_.program -eq "tmp-p1.exe" }
    Check ($pp1.enabled -eq "on" -and $pp1.priority -eq "belownormal") "previous state has p1 on, below normal"
    Check (@($pf.profiles).Count -eq 2) "both profiles are still stored"

    # ---- run 2: revert
    $o = Run "profiles-revert" 5 "" @("wait:3", "$cmd,3", "wait:2")
    $r = Rules
    $p1 = $r | Where-Object { $_.program -eq "tmp-p1.exe" }; $p2 = $r | Where-Object { $_.program -eq "tmp-p2.exe" }
    Check ($p1.enabled -and $p1.startPriority -eq 5) "revert: p1 back on, below normal"
    Check (-not $p2.enabled -and $p2.startPriority -eq 0) "revert: p2 back off, no priority"
    Check ((Theme) -eq 2) "revert: theme back to dark (2)"
    $pf = Get-Content $profilesFile -Raw -Encoding UTF8 | ConvertFrom-Json
    Check ($null -eq $pf.previous -and $pf.active -eq "") "revert: the previous snapshot is consumed, no active profile"

    # ---- run 3: save current, export, import
    Write-Json $importFile @{ version = 1; profiles = @(
        @{ name = "Imported"; theme = "dark"; rules = @(@{ program = "tmp-p1.exe"; enabled = "on" }) },
        @{ name = ""; rules = @() },
        @{ name = "BadTheme"; theme = "purple"; rules = @(@{ program = "tmp-p1.exe"; enabled = "on" }) },
        @{ notaprofile = 1 }) }
    $script:currentFile = $exportFile
    Write-Settings 2 "Saved1"
    $o = & $runHidden -Name profiles-save -Settings $settings -Wait 9 -Wait2 1 -Steps @("wait:3", "$cmd,1", "wait:1", "$cmd,4", "wait:1") -ExitCmd | Out-String
    $ex = Get-Content $exportFile -Raw -Encoding UTF8 | ConvertFrom-Json
    Check (@($ex.profiles).Count -eq 3 -and ($ex.profiles | Where-Object { $_.name -eq "Saved1" })) "save current as 'Saved1' then export all: 3 profiles in the exported file"
    $s1 = $ex.profiles | Where-Object { $_.name -eq "Saved1" }
    Check ($s1.theme -eq "dark" -and @($s1.rules).Count -eq 3 -and ($s1.rules | Where-Object { $_.program -eq "tmp-p3.exe" }).priority -eq "idle") "Saved1 captured theme dark, three rules, p3 priority idle"
    $script:currentFile = $importFile
    $o = Run "profiles-import" 2 "" @("wait:3", "$cmd,5", "wait:1")
    $pf = Get-Content $profilesFile -Raw -Encoding UTF8 | ConvertFrom-Json
    $names = @($pf.profiles | ForEach-Object { $_.name })
    Check (($names -contains "Imported") -and -not ($names -contains "BadTheme") -and -not ($names -contains "")) "import: the valid profile was added, the empty-name, bad-theme and non-profile entries were skipped ($($names -join ', '))"
    Check ($names -contains "Gaming" -and $names -contains "Saved1") "import keeps the existing profiles"

    # ---- screenshots
    foreach ($t in @(@("dark", 2), @("light", 1), @("oled", 5))) {
        $o = Run "profiles-manage-$($t[0])" $t[1] "" @("wait:3", "$cmd,6", "wait:2", "dialog:yes", "wait:1")
        Check ($o -match "dialog answered yes" -and (Test-Path (Join-Path $shots "profiles-manage-$($t[0])-dialog.png"))) "Manage profiles dialog opened ($($t[0]) theme)"
    }
    $o = Run "profiles-save-dialog" 2 "" @("wait:3", "$cmd,7", "wait:2", "dialog:no", "wait:1")
    Check ($o -match "dialog answered no") "Save profile dialog opened and cancelled"
    $o = & $runHidden -Name profiles-palette -Settings $settings -Wait 9 -Wait2 1 -Width 1100 -Height 800 -Steps @("msg:TaskManagerPlus.QuickActions.Open", "wait:1", "type:profile", "wait:1", "shot:palette-profiles") | Out-String
    if ($o -match "run folder: (.+)") { Get-ChildItem $Matches[1].Trim() -Filter "palette-profiles.png" | ForEach-Object { Copy-Item $_.FullName (Join-Path $shots $_.Name) -Force } }
    Check ((Test-Path (Join-Path $shots "palette-profiles.png")) -and $o -notmatch "step (shot|type): no ") "palette lists profile commands (screenshot palette-profiles.png)"
}
finally {
    & $runHidden -Cleanup | Out-Null
}

"screenshots: $shots"
if ($fail) { "profiles test FAILED ($fail)"; exit 1 }
"profiles test passed"
