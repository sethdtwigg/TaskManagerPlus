<#
.SYNOPSIS
  Hidden-desktop test for customizable keyboard shortcuts (Options > Shortcuts). Never touches the user's desktop or settings.

.DESCRIPTION
  Uses tools\run-hidden.ps1 with scratch settings. A real Ctrl+key cannot be sent to a window on another desktop (posted key
  messages carry no modifier state), so shortcuts are exercised through the registered message
  "TaskManagerPlus.Shortcuts.Dispatch" (wParam = SHC_MOD bits 1 Ctrl / 2 Shift / 4 Alt, lParam = virtual key), which looks the
  combination up in the live table and posts the bound command to the main window, the same lookup the accelerator table is
  built from. The Win32 accelerator translation itself and the QuickActions Ctrl+K hook are not driven by it.
  1. Defaults: Ctrl+R opens the Run dialog.
  2. A rebinding in the settings file ("run=Ctrl+J"): Ctrl+R does nothing, Ctrl+J opens Run.
  3. Damaged settings ("run=Ctrl+C;refresh=banana"): the defaults survive.
  4. Options > Shortcuts: typing a taken shortcut, a reserved one, a bare letter and garbage each show the right message
     and change nothing; a valid one is accepted and saved (Shortcuts=find=Ctrl+Shift+F8 in the saved settings); Reset all
     returns to the defaults (empty setting).
  5. Screenshots of the page in Light, Dark and OLED (folder $env:TEMP\tmplus-shortcuts-test\shots).

.EXAMPLE
  powershell -File tools\tests\shortcuts-hidden.ps1
#>
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runHidden = Join-Path $root "tools\run-hidden.ps1"
$dir = Join-Path $env:TEMP "tmplus-shortcuts-test"
$shots = Join-Path $dir "shots"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $shots | Out-Null

$fail = 0
function Check($ok, $text) { if ($ok) { "PASS  $text" } else { "FAIL  $text"; $script:fail++ } }

function Write-Settings($name, $themeMode, $shortcuts) {
    $h = @{ '$schema' = "settings.schema.json"; "EnableThemeSupport" = "1"; "ThemeMode" = "$themeMode"; "ReadableLookOffered" = "1"; "CalmLookOffered" = "1" }
    if ($shortcuts -ne $null) { $h["Shortcuts"] = $shortcuts }
    $path = Join-Path $dir "$name.json"
    [IO.File]::WriteAllText($path, ($h | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
    return $path
}

function Invoke-Run($name, $settings, $steps, $options = $false, $capture = $false) {
    if ($options) { $env:TMPLUS_TEST_OPTIONS_SECTION = "Shortcuts" }
    try {
        $p = @{ Name = $name; Settings = $settings; Wait = 8; Wait2 = 1; Steps = $steps; Width = 1100; Height = 800; ExitCmd = $true }
        if ($options) { $p.Commands = @(10083); $p.Wait2 = 3 }
        if ($capture) { $p.CaptureDialog = $true }
        $out = & $runHidden @p | Out-String
    } finally { Remove-Item env:TMPLUS_TEST_OPTIONS_SECTION -ErrorAction SilentlyContinue }
    $script:lastOut = $out
    if ($out -match "run folder: (.+)") {
        Get-ChildItem $Matches[1].Trim() -Filter *.png | ForEach-Object { Copy-Item $_.FullName (Join-Path $shots $_.Name) -Force }
    }
}

function Get-Setting($settingsPath, $key) {
    $m = [regex]::Match((Get-Content $settingsPath -Raw), "`"$key`":\s*`"([^`"]*)`"")
    if ($m.Success) { return $m.Groups[1].Value } else { return $null }
}

$dispatch = "msg:TaskManagerPlus.Shortcuts.Dispatch"
$ctrlR = "$dispatch,1,82"
$ctrlJ = "$dispatch,1,74"

# 1. defaults
$s = Write-Settings "defaults" 2 $null
Invoke-Run "defaults" $s @($ctrlR, "wait:2", "dialog:no", "wait:1")
Check ($script:lastOut -match "dialog answered no") "default: Ctrl+R opens the Run dialog"

# 2. rebinding from the settings file
$s = Write-Settings "rebound" 2 "run=Ctrl+J"
Invoke-Run "rebound" $s @($ctrlR, "wait:2", "dialog:no", "wait:1", $ctrlJ, "wait:2", "dialog:no", "wait:1")
$answers = @($script:lastOut -split "`r?`n" | Where-Object { $_ -match "step dialog: none|dialog answered no" })
Check ($answers.Count -eq 2 -and $answers[0] -match "none" -and $answers[1] -match "answered") "rebound run=Ctrl+J: Ctrl+R does nothing, Ctrl+J opens the Run dialog"

# 3. damaged settings fall back to the defaults
$s = Write-Settings "damaged" 2 "run=Ctrl+C;refresh=banana;;=;nosuch=Ctrl+Q"
Invoke-Run "damaged" $s @($ctrlR, "wait:2", "dialog:no", "wait:1")
Check ($script:lastOut -match "dialog answered no") "damaged settings: reserved / unparsable entries are ignored, Ctrl+R still opens Run"

# 4. the Options page
$s = Write-Settings "page" 2 $null
$steps = @(
    "wait:1",
    "settext:1584,Ctrl+R", "click:1585", "gettext:1589",
    "settext:1584,Ctrl+C", "click:1585", "gettext:1589",
    "settext:1584,K", "click:1585", "gettext:1589",
    "settext:1584,banana", "click:1585", "gettext:1589",
    "settext:1584,Ctrl+Shift+F8", "click:1585", "gettext:1589"
)
Invoke-Run "page" $s $steps $true
$texts = @($script:lastOut -split "`r?`n" | Where-Object { $_ -match "^text 1589:" })
Check ($texts.Count -eq 5) "page: five status messages read back"
if ($texts.Count -eq 5) {
    Check ($texts[0] -match 'already used by "Run\.\.\."') "page: a taken shortcut names its owner ($($texts[0]))"
    Check ($texts[1] -match "reserved") "page: Ctrl+C is reserved ($($texts[1]))"
    Check ($texts[2] -match "interfere with typing") "page: a bare letter is refused ($($texts[2]))"
    Check ($texts[3] -match "not a shortcut") "page: garbage is refused ($($texts[3]))"
    Check ($texts[4] -match "is now Ctrl\+Shift\+F8") "page: a valid shortcut is accepted ($($texts[4]))"
}
Check ((Get-Setting $s "Shortcuts") -eq "find=Ctrl+Shift+F8") "page: only the accepted change is saved (Shortcuts = $(Get-Setting $s 'Shortcuts'))"

# unbind and per-row default
$s = Write-Settings "unbind" 2 "find=Ctrl+Shift+F8;log=Alt+L"
Invoke-Run "unbind" $s @("wait:1", "click:1586", "gettext:1589", "click:1587", "gettext:1589") $true
$texts = @($script:lastOut -split "`r?`n" | Where-Object { $_ -match "^text 1589:" })
Check ($texts.Count -eq 2 -and $texts[0] -match "is now None" -and $texts[1] -match "is now Ctrl\+F") "page: Unbind then Default on the first row ($($texts -join ' | '))"
Check ((Get-Setting $s "Shortcuts") -eq "log=Alt+L") "page: saved value keeps only the other customization (Shortcuts = $(Get-Setting $s 'Shortcuts'))"

# reset all
$s = Write-Settings "resetall" 2 "find=Ctrl+Shift+F8;log=Alt+L;quickactions=Ctrl+Shift+K"
Invoke-Run "resetall" $s @("wait:1", "click:1588", "gettext:1589") $true
Check ($script:lastOut -match "text 1589: All shortcuts are back to their defaults") "page: Reset all reports success"
Check ([string]::IsNullOrEmpty((Get-Setting $s "Shortcuts"))) "page: Reset all leaves an empty Shortcuts setting (got '$(Get-Setting $s 'Shortcuts')')"

# 5. screenshots with customizations, three themes
foreach ($t in @(@("light", 1), @("dark", 2), @("oled", 5))) {
    $s = Write-Settings "shot-$($t[0])" $t[1] "quickactions=Ctrl+Shift+K;refresh=None;log=Alt+L"
    Invoke-Run "shortcuts-$($t[0])" $s @("wait:1") $true $true
    Check (Test-Path (Join-Path $shots "shortcuts-$($t[0])-dialog.png")) "Options > Shortcuts screenshot, $($t[0]) theme (shortcuts-$($t[0])-dialog.png)"
}

& $runHidden -Cleanup | Out-Null
if ($fail) { "shortcuts hidden test: $fail FAILED"; exit 1 }
"shortcuts hidden test passed; screenshots in $shots"
