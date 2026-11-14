<#
.SYNOPSIS
  Hidden-desktop screenshot sweep (Phase 6b / release plan B6): main window, every tab, QuickActions palette, About and
  Options in Light, Dark and OLED at 100/125/150/200% DPI. Never touches the user's settings or desktop.

.DESCRIPTION
  Uses tools\run-hidden.ps1 (scratch settings, invisible desktop). Output goes to -Out (default %TEMP%\tmplus-ui-sweep) as
  <theme>-<dpi>-<what>.png, because run-hidden deletes its own run folders at each start.

  DPI: a hidden desktop cannot change the real display scale. This script sets TMPLUS_TEST_DPI, which only a TEMPORARY
  build understands (the TMPLUS-TEMP-DPI hook in phlib\guisup.c PhGetDpiValue, never committed). With a normal build the
  variable is ignored and every capture is at the real DPI (96 on the dev machine). The title bar and menu are drawn by
  Windows at the real DPI either way; what scales is everything the app lays out itself.

.EXAMPLE
  powershell -File tools\tests\ui-sweep-hidden.ps1 -Themes dark -Dpis 96,192 -Out C:\x\shots
#>
param(
    [string[]]$Themes = @("light", "dark", "oled"),
    [int[]]$Dpis = @(96, 120, 144, 192),
    [int]$Tabs = 12,
    [string]$Out = (Join-Path $env:TEMP "tmplus-ui-sweep")
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runHidden = Join-Path $root "tools\run-hidden.ps1"
$work = Join-Path $env:TEMP "tmplus-ui-sweep-work"
New-Item -ItemType Directory -Force -Path $Out, $work | Out-Null
$modes = @{ light = 1; dark = 2; oled = 5 }

function Write-Settings($mode, $path) {
    $h = @{ '$schema' = "settings.schema.json"; "EnableThemeSupport" = "1"; "ThemeMode" = "$mode"; "ReadableLookOffered" = "1"
            "CalmLookOffered" = "1"; "MainWindowHiddenTabs" = "" }
    [IO.File]::WriteAllText($path, ($h | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
}

function Run($tag, $runArgs) {
    $o = ""
    foreach ($try in 1..3) {   # a launch right after the previous run sometimes finds no main window; retry
        try { $o = & $runHidden @runArgs | Out-String; break } catch { "retry $tag ($try): $_"; & $runHidden -Cleanup | Out-Null; Start-Sleep -Seconds 4 }
    }
    if ($o -match "run folder: (.+)") {
        $dir = $Matches[1].Trim()
        Get-ChildItem $dir -Filter *.png | ForEach-Object { Copy-Item $_.FullName (Join-Path $Out ("$tag-" + $_.Name)) -Force }
    } else { "NO RUN FOLDER for $tag`n$o" }
}

foreach ($t in $Themes) {
    foreach ($d in $Dpis) {
        $env:TMPLUS_TEST_DPI = "$d"
        $tag = "$t-$d"
        $settings = Join-Path $work "$tag.json"
        Write-Settings $modes[$t] $settings
        $steps = @("wait:2", "shotmain:tab00")
        for ($i = 1; $i -lt $Tabs; $i++) { $steps += "cmdshot:10223,tab{0:00}" -f $i }
        $steps += @("msg:TaskManagerPlus.QuickActions.Open", "wait:2", "type:chro", "wait:1", "shot:palette")
        Run $tag @{ Name = "end"; Settings = $settings; Wait = 12; Wait2 = 1; Steps = $steps; Width = 1100; Height = 800 }
        Run "$tag-about" @{ Name = "about"; Settings = $settings; Wait = 12; Wait2 = 1; Commands = @(10106); CaptureDialog = $true; Width = 1100; Height = 800 }
        Run "$tag-options" @{ Name = "options"; Settings = $settings; Wait = 12; Wait2 = 1; Commands = @(10083); CaptureDialog = $true; Width = 1100; Height = 800 }
        "done $tag"
    }
}
Remove-Item env:TMPLUS_TEST_DPI -ErrorAction SilentlyContinue
& $runHidden -Cleanup
"shots in $Out"
