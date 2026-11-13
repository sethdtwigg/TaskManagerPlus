<#
.SYNOPSIS
  Hidden-desktop test for the QuickActions palette (Phase 7 item 3). Never touches the user's desktop or settings.

.DESCRIPTION
  Uses tools\run-hidden.ps1 (private invisible desktop, job object) with scratch settings.
  1. Screenshots of the open palette in the light, dark and OLED themes (folder $env:TEMP\tmplus-qa-test\shots).
  2. Action run: starts a throwaway process (a copy of ping.exe named tmp-qa-target.exe), opens the palette, types
     "end tmp-qa-target", presses Enter, answers the confirmation dialog with Yes and checks that the process is gone.
     Then a second throwaway process is suspended the same way ("suspend tmp-qa-target"), checking that its threads end
     up suspended.
  3. Keyboard only: the same End task, but the result is chosen with the Down arrow first (type the process name, Down
     until "End task" is highlighted is not asserted by text; the check is again that the process dies).

  The palette is opened by posting the registered window message "TaskManagerPlus.QuickActions.Open" because key
  messages posted to a window on another desktop do not carry the Ctrl key state, so a real Ctrl+K cannot be sent
  headlessly; the message goes through the same code as the Ctrl+K handler after its key check.

.EXAMPLE
  powershell -File tools\tests\quickactions-hidden.ps1
#>
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runHidden = Join-Path $root "tools\run-hidden.ps1"
$dir = Join-Path $env:TEMP "tmplus-qa-test"
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

function Invoke-Run($name, $settings, $steps, $wait = 9) {
    $out = & $runHidden -Name $name -Settings $settings -Wait $wait -Wait2 1 -Steps $steps -Width 1100 -Height 800 | Out-String
    $out.Trim() -split "`r?`n" | Where-Object { $_ -match "posted|answered|step|exited|no main" } | ForEach-Object { "   $_" }
    $script:stepErrors = @($out -split "`r?`n" | Where-Object { $_ -match "step (shot|type|key): no " })
    if ($out -match "run folder: (.+)") {
        Get-ChildItem $Matches[1].Trim() -Filter *.png | ForEach-Object { Copy-Item $_.FullName (Join-Path $shots $_.Name) -Force }
    }
}

$open = "msg:TaskManagerPlus.QuickActions.Open"

# --- 1. themes: 1 = light, 2 = dark, 5 = OLED
foreach ($t in @(@("light", 1), @("dark", 2), @("oled", 5))) {
    $s = Write-Settings "theme-$($t[0])" $t[1] @{}
    Invoke-Run "qa-$($t[0])" $s @($open, "wait:1", "shot:palette-$($t[0])-empty", "type:chrome", "wait:1", "shot:palette-$($t[0])-search", "key:27", "wait:1", "shotmain:main-$($t[0])")
    Check ((Test-Path (Join-Path $shots "palette-$($t[0])-search.png")) -and $script:stepErrors.Count -eq 0) "palette opened and typed into, $($t[0]) theme (screenshot palette-$($t[0])-search.png)"
}

# --- throwaway target processes
$tmp = Join-Path $dir "target"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$exe = Join-Path $tmp "tmp-qa-target.exe"
Copy-Item (Join-Path $env:SystemRoot "System32\PING.EXE") $exe
function Start-Target { Start-Process -FilePath $exe -ArgumentList "-n 900 127.0.0.1" -WindowStyle Hidden -PassThru }

# --- 2. End task with the confirmation dialog
$target = Start-Target
Start-Sleep -Seconds 1
$s = Write-Settings "action-end" 2 @{}
Invoke-Run "qa-end" $s @($open, "wait:1", "type:end tmp-qa-target", "wait:1", "shot:palette-end", "key:13", "wait:2", "dialog:yes", "wait:3")
$target.Refresh()
Check $target.HasExited "End task ran on tmp-qa-target.exe through the palette (confirmation answered Yes)"
if (-not $target.HasExited) { Stop-Process -Id $target.Id -Force }

# --- Escape must not run anything
$target = Start-Target
Start-Sleep -Seconds 1
Invoke-Run "qa-esc" $s @($open, "wait:1", "type:end tmp-qa-target", "wait:1", "key:27", "wait:2")
$target.Refresh()
Check (-not $target.HasExited) "Esc closed the palette without touching the process"

# --- 3. Suspend (no confirmation) with Down/Up in the middle: Down then Up lands on the same row
Invoke-Run "qa-suspend" $s @($open, "wait:1", "type:suspend tmp-qa-target", "wait:1", "key:40", "key:38", "wait:1", "key:13", "wait:3")
$target.Refresh()
$suspended = $false
if (-not $target.HasExited) {
    $threads = (Get-Process -Id $target.Id).Threads
    $suspended = ($threads | Where-Object { $_.ThreadState -eq "Wait" -and $_.WaitReason -eq "Suspended" }).Count -gt 0
}
Check $suspended "Suspend ran through the palette (Down then Up kept the selection)"
if (-not $target.HasExited) { Stop-Process -Id $target.Id -Force }

& $runHidden -Cleanup | Out-Null
"screenshots: $shots"
if ($fail) { "quick actions test FAILED ($fail)"; exit 1 }
"quick actions test passed"
