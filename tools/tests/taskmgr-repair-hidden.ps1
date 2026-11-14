<#
.SYNOPSIS
  Hidden-desktop test for the Task Manager replacement repair prompt (TaskManagerPlus). Never touches the user's desktop,
  settings or the machine's Image File Execution Options key.

.DESCRIPTION
  The prompt is driven by the test hook TMPLUS_TEST_TASKMGR_DEBUGGER (the Debugger value the app should act on) instead of
  the real HKLM key, so nothing is written to the registry here. The Repair button is never clicked (it would start a UAC
  prompt; TMPLUS_TEST_TASKMGR_ANSWER only knows notnow and never); the elevated write is covered by the decision unit tests (test-tmrepair.c) and, by hand, in the doc.
  1. Screenshots of the prompt in Light, Dark and OLED (folder $env:TEMP\tmplus-tmr-test\shots).
  2. "Not now" leaves the setting alone (the prompt returns next start).
  3. "Don't ask again" saves TaskmgrRepairNever=1 and the next start shows no prompt.
  4. No prompt for another program's debugger, for a path that still exists, or when the value already points here.

.EXAMPLE
  powershell -File tools\tests\taskmgr-repair-hidden.ps1
#>
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runHidden = Join-Path $root "tools\run-hidden.ps1"
$dir = Join-Path $env:TEMP "tmplus-tmr-test"
$shots = Join-Path $dir "shots"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $shots | Out-Null

$fail = 0
function Check($ok, $text) { if ($ok) { "PASS  $text" } else { "FAIL  $text"; $script:fail++ } }

function Write-Settings($name, $themeMode, $extra) {
    $h = @{ '$schema' = "settings.schema.json"; "EnableThemeSupport" = "1"; "ThemeMode" = "$themeMode"; "ReadableLookOffered" = "1"; "CalmLookOffered" = "1" }
    foreach ($k in $extra.Keys) { $h[$k] = $extra[$k] }
    $path = Join-Path $dir "$name.json"
    [IO.File]::WriteAllText($path, ($h | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
    return $path
}

function Invoke-Run($name, $settings, $debugger, $steps, $exit = $false, $answer = "") {
    $env:TMPLUS_TEST_TASKMGR_DEBUGGER = $debugger
    if ($answer) { $env:TMPLUS_TEST_TASKMGR_ANSWER = $answer }
    try {
        $args2 = @{ Name = $name; Settings = $settings; Wait = 8; Wait2 = 1; Steps = $steps; Width = 1100; Height = 800 }
        if ($exit) { $args2.ExitCmd = $true }
        $out = & $runHidden @args2 | Out-String
    } finally { Remove-Item env:TMPLUS_TEST_TASKMGR_DEBUGGER -ErrorAction SilentlyContinue; Remove-Item env:TMPLUS_TEST_TASKMGR_ANSWER -ErrorAction SilentlyContinue }
    $script:lastOut = $out
    if ($out -match "run folder: (.+)") {
        $rf = $Matches[1].Trim()
        if (Test-Path (Join-Path $rf "step-dialog.png")) { Copy-Item (Join-Path $rf "step-dialog.png") (Join-Path $shots "$name-prompt.png") -Force }
    }
}

$stale = '"C:\Old Portable\TaskManagerPlus.exe"'

# 1. prompt in three themes; answer Not now (IDCANCEL = 2)
foreach ($t in @(@("light", 1), @("dark", 2), @("oled", 5))) {
    $s = Write-Settings "theme-$($t[0])" $t[1] @{}
    Invoke-Run "prompt-$($t[0])" $s $stale @("wait:1", "dialogcmd:2", "wait:1")
    Check ($script:lastOut -match "dialog command 2" -and (Test-Path (Join-Path $shots "prompt-$($t[0])-prompt.png"))) "repair prompt shown, $($t[0]) theme (prompt-$($t[0])-prompt.png)"
}

# 2. Not now leaves the setting alone
$s = Write-Settings "notnow" 2 @{}
Invoke-Run "notnow" $s $stale @("wait:1") $true "notnow"
$saved = Get-Content $s -Raw
Check ($saved -notmatch '"TaskmgrRepairNever":\s*"?[1-9]') "Not now does not set TaskmgrRepairNever"

# 3. Don't ask again (IDNO = 7) is saved, and the next start shows no prompt
$s = Write-Settings "never" 2 @{}
Invoke-Run "never1" $s $stale @("wait:1") $true "never"
$saved = Get-Content $s -Raw
Check ($saved -match '"TaskmgrRepairNever":\s*"?1') "Don't ask again saved TaskmgrRepairNever=1"
Invoke-Run "never2" $s $stale @("wait:1", "dialogcmd:2")
Check ($script:lastOut -match "step dialogcmd: none") "no prompt on the next start after Don't ask again"

# 4. no prompt when it is not ours / still exists / empty
$s = Write-Settings "quiet" 2 @{}
Invoke-Run "foreign" $s '"C:\Program Files\Other\procexp64.exe"' @("wait:1", "dialogcmd:2")
Check ($script:lastOut -match "step dialogcmd: none") "no prompt for another program's debugger"
$copy = Join-Path $dir "TaskManagerPlus.exe"
Copy-Item (Join-Path $env:SystemRoot "System32\PING.EXE") $copy
Invoke-Run "exists" $s "`"$copy`"" @("wait:1", "dialogcmd:2")
Check ($script:lastOut -match "step dialogcmd: none") "no prompt when the stored TaskManagerPlus path still exists"
Invoke-Run "empty" $s "" @("wait:1", "dialogcmd:2")
Check ($script:lastOut -match "step dialogcmd: none") "no prompt without a stored value (the real key is not read when the hook is empty and absent here)"

& $runHidden -Cleanup | Out-Null
if ($fail) { "taskmgr repair hidden test: $fail FAILED"; exit 1 }
"taskmgr repair hidden test passed; screenshots in $shots"
