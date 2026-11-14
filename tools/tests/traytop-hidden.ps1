<#
.SYNOPSIS
  Hidden-desktop test for the tray quick-menu of top processes (TrayTop plugin). Never touches the user's desktop or settings.

.DESCRIPTION
  Uses tools\run-hidden.ps1 with scratch settings. There is no taskbar on the hidden desktop, so the tray icon cannot be
  right-clicked; the menu is exercised two ways:
  * the real path: the tray icon's callback message (WM_APP+126, WM_RBUTTONUP, icon id in the high word) is posted to the main
    window, which runs PhShowIconContextMenu and the plugin's menu hook (screenshot of the whole tray menu with "Top processes");
  * the plugin's own test message "TaskManagerPlus.TrayTop.Show" pops just the Top processes list (screenshots, in
    Light/Dark/OLED, of the list and of a process's action submenu), and "TaskManagerPlus.TrayTop.Action" runs an action on a
    pid exactly as a menu click does.
  Checks: End task on a throwaway process (confirmation answered Yes) ends it; Efficiency mode on another does not crash
  anything; an unknown pid is ignored; the options page clamps the count (99 -> 15, 0 -> 5, 7 stays) and the checkbox
  saves. The ranking itself is unit tested (test-traytop.c). A real right-click on a real tray icon is not covered.

.EXAMPLE
  powershell -File tools\tests\traytop-hidden.ps1
#>
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runHidden = Join-Path $root "tools\run-hidden.ps1"
$dir = Join-Path $env:TEMP "tmplus-traytop-test"
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

function Invoke-Run($name, $settings, $steps, $options = $false, $stepClass = "PhEMenuDX") {
    if ($options) { $env:TMPLUS_TEST_OPTIONS_SECTION = "Tray top processes" }
    try {
        $p = @{ Name = $name; Settings = $settings; Wait = 9; Wait2 = 1; Steps = $steps; Width = 1100; Height = 800; ExitCmd = $true; StepClass = $stepClass }
        if ($options) { $p.Commands = @(10083); $p.Wait2 = 3 }
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

# a CPU hog so the lists are not empty
$hog = Start-Process -FilePath "powershell.exe" -ArgumentList "-NoProfile", "-WindowStyle", "Hidden", "-Command", "while(`$true){}" -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 3
$target = Join-Path $dir "tmp-tt-target.exe"
Copy-Item (Join-Path $env:SystemRoot "System32\PING.EXE") $target
function Start-Target { Start-Process -FilePath $target -ArgumentList "-n 900 127.0.0.1" -WindowStyle Hidden -PassThru }

try {
    $show = "msg:TaskManagerPlus.TrayTop.Show,200,120"
    $action = "msg:TaskManagerPlus.TrayTop.Action"

    # 1. screenshots in three themes
    foreach ($t in @(@("light", 1), @("dark", 2), @("oled", 5))) {
        $s = Write-Settings "menu-$($t[0])" $t[1] @{ "TaskManagerPlus.TrayTop.Count" = "4" }
        Invoke-Run "menu-$($t[0])" $s @($show, "wait:2", "shotall:menu-$($t[0])", "menukey:40", "menukey:39", "wait:1", "shotall:submenu-$($t[0])", "menukey:27", "menukey:27", "wait:1")
        Check ((Test-Path (Join-Path $shots "menu-$($t[0])-0.png")) -and (Test-Path (Join-Path $shots "submenu-$($t[0])-1.png"))) "Top processes list and action submenu captured, $($t[0]) theme (menu-$($t[0])-0.png, submenu-$($t[0])-1.png)"
    }

    # 2. the real tray path: the icon callback message with a right-button-up for icon ids 1..4
    $s = Write-Settings "tray" 2 @{ "IconSettings" = "1|1|2|1" }
    $post = @("wait:2")
    foreach ($id in 1..4) { $post += "post:32894,0,$(($id * 65536) + 517)"; $post += "wait:1" }
    Invoke-Run "tray" $s ($post + @("wait:1", "shotall:tray-menu", "menukey:27", "menukey:27", "wait:1"))
    Check (Test-Path (Join-Path $shots "tray-menu-0.png")) "the tray icon's right-click path shows the real tray menu (tray-menu-0.png)"

    # 3. End task through the action message
    $victim = Start-Target
    Start-Sleep -Seconds 1
    $s = Write-Settings "end" 2 @{}
    Invoke-Run "end" $s @("$action,$($victim.Id),1", "wait:2", "dialog:yes", "wait:3")
    $victim.Refresh()
    Check $victim.HasExited "End task through the menu action ends the process (confirmation answered Yes)"
    if (-not $victim.HasExited) { Stop-Process -Id $victim.Id -Force }

    # Esc / No keeps it
    $keep = Start-Target
    Start-Sleep -Seconds 1
    Invoke-Run "endno" $s @("$action,$($keep.Id),1", "wait:2", "dialog:no", "wait:2")
    $keep.Refresh()
    Check (-not $keep.HasExited) "answering No to the End task confirmation leaves the process alone"

    # 4. efficiency mode and an unknown pid do not take the app down
    Invoke-Run "eco" $s @("$action,$($keep.Id),2", "wait:2", "$action,4000000,2", "$action,4000000,1", "$action,0,3", "wait:2", "shotmain:after")
    Check ($script:lastOut -match "exited=True" -and (Test-Path (Join-Path $shots "after.png"))) "Efficiency mode on a pid and actions on unknown pids leave the app running"
    if (-not $keep.HasExited) { Stop-Process -Id $keep.Id -Force }

    # 5. options page: clamping and the on/off checkbox
    foreach ($case in @(@("99", "f"), @("7", "7"), @("0", "5"), @("1", "1"))) {
        $s = Write-Settings "opt-$($case[0])" 2 @{}
        Invoke-Run "opt-$($case[0])" $s @("wait:1", "settext:40102,$($case[0])", "closedialog", "wait:1") $true "#32770"
        $saved = Get-Setting $s "TaskManagerPlus.TrayTop.Count"
        Check ($saved -eq $case[1]) "options: count '$($case[0])' is saved as $($case[1]) (got '$saved')"
    }
    $s = Write-Settings "opt-off" 2 @{}
    Invoke-Run "opt-off" $s @("wait:1", "click:40101", "closedialog", "wait:1") $true "#32770"
    Check ((Get-Setting $s "TaskManagerPlus.TrayTop.Enabled") -eq "0") "options: unticking the checkbox saves Enabled = 0"
    $s = Write-Settings "opt-shot" 1 @{}
    $env:TMPLUS_TEST_OPTIONS_SECTION = "Tray top processes"
    try { $out = & $runHidden -Name "opt-shot" -Settings $s -Wait 9 -Wait2 3 -Commands @(10083) -CaptureDialog -Width 1100 -Height 800 | Out-String } finally { Remove-Item env:TMPLUS_TEST_OPTIONS_SECTION -ErrorAction SilentlyContinue }
    if ($out -match "run folder: (.+)") { Get-ChildItem $Matches[1].Trim() -Filter *.png | ForEach-Object { Copy-Item $_.FullName (Join-Path $shots $_.Name) -Force } }
    Check (Test-Path (Join-Path $shots "opt-shot-dialog.png")) "options page screenshot, Light (opt-shot-dialog.png)"
}
finally {
    if (-not $hog.HasExited) { Stop-Process -Id $hog.Id -Force }
    & $runHidden -Cleanup | Out-Null
}

if ($fail) { "traytop hidden test: $fail FAILED"; exit 1 }
"traytop hidden test passed; screenshots in $shots"
