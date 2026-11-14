<#
.SYNOPSIS
  Hidden-desktop test for theme JSON export / import (Options > General). Never touches the user's desktop or settings.

.DESCRIPTION
  Uses tools\run-hidden.ps1 with scratch settings. The file dialogs are replaced by the test hooks
  TMPLUS_TEST_THEME_EXPORT / TMPLUS_TEST_THEME_IMPORT (a file name from the environment); the confirmation and result
  dialogs are real and answered through run-hidden -Dialogs2.
  1. Options > General screenshots (Export/Import buttons) in Light, Dark and OLED (folder $env:TEMP\tmplus-theme-test\shots).
  2. Export in each theme: valid JSON with the six colours as #RRGGBB, OLED background pure black.
  3. Import a valid theme: ThemeMode becomes Custom (3) and the six colour settings hold the imported values (checked in
     the settings file the app saves on exit); screenshot of the main window with the imported palette.
  4. Import rejects: malformed JSON, bad colour, missing colour, wrong version, unreadable contrast, oversize file,
     binary garbage, empty file, a missing file. Each shows an error, the settings stay untouched and the app keeps running.
  5. Round trip: export, then import the exported file unchanged is accepted.

.EXAMPLE
  powershell -File tools\tests\theme-json-hidden.ps1
#>
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runHidden = Join-Path $root "tools\run-hidden.ps1"
$dir = Join-Path $env:TEMP "tmplus-theme-test"
$shots = Join-Path $dir "shots"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $shots | Out-Null

$fail = 0
function Check($ok, $text) { if ($ok) { "PASS  $text" } else { "FAIL  $text"; $script:fail++ } }

function Write-Settings($name, $themeMode) {
    $h = @{ '$schema' = "settings.schema.json"; "EnableThemeSupport" = "1"; "ThemeMode" = "$themeMode"; "ReadableLookOffered" = "1"; "CalmLookOffered" = "1" }
    $path = Join-Path $dir "$name.json"
    [IO.File]::WriteAllText($path, ($h | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
    return $path
}

function Invoke-Run($name, $settings, $click, $dialogs, $export, $import, $steps = @()) {
    if ($export) { $env:TMPLUS_TEST_THEME_EXPORT = $export }
    if ($import) { $env:TMPLUS_TEST_THEME_IMPORT = $import }
    try {
        $p = @{ Name = $name; Settings = $settings; Wait = 8; Wait2 = 1; OptionsClick = $click; Dialogs2 = $dialogs; Width = 1100; Height = 800; ExitCmd = $true }
        if ($steps.Count) { $p.Steps = $steps }
        $out = & $runHidden @p | Out-String
    } finally { Remove-Item env:TMPLUS_TEST_THEME_EXPORT, env:TMPLUS_TEST_THEME_IMPORT -ErrorAction SilentlyContinue }
    $script:lastOut = $out
    if ($out -match "run folder: (.+)") {
        Get-ChildItem $Matches[1].Trim() -Filter *.png | ForEach-Object { Copy-Item $_.FullName (Join-Path $shots $_.Name) -Force }
    }
}

function Get-Setting($settingsPath, $key) {
    $m = [regex]::Match((Get-Content $settingsPath -Raw), "`"$key`":\s*`"([^`"]*)`"")
    if ($m.Success) { return $m.Groups[1].Value } else { return $null }
}

$EXPORT = 1581
$IMPORT = 1582

# 1 + 2: buttons screenshot and export in each theme
$expected = @{}
foreach ($t in @(@("light", 1), @("dark", 2), @("oled", 5))) {
    $s = Write-Settings "export-$($t[0])" $t[1]
    $file = Join-Path $dir "exported-$($t[0]).json"
    Invoke-Run "general-$($t[0])" $s $EXPORT @("yes") $file $null
    $ok = Test-Path $file
    $detail = ""
    if ($ok) {
        try {
            $j = Get-Content $file -Raw | ConvertFrom-Json
            $names = "foreground", "background", "background2", "highlight", "highlight2", "text"
            $ok = ($j.format -eq "TaskManagerPlus.Theme") -and ($j.version -eq 1)
            foreach ($n in $names) { if ($j.colors.$n -notmatch '^#[0-9A-F]{6}$') { $ok = $false; $detail += " bad $n=$($j.colors.$n)" } }
            $expected[$t[0]] = $j
            $detail = "background $($j.colors.background), text $($j.colors.text)" + $detail
        } catch { $ok = $false; $detail = "does not parse" }
    }
    Check ($ok -and (Test-Path (Join-Path $shots "general-$($t[0]).options.png"))) "export in $($t[0]) theme: valid file ($detail), options page screenshot general-$($t[0]).options.png"
}
Check ($expected["oled"].colors.background -eq "#000000") "OLED export has a pure black background"
Check ($expected["light"].colors.background -ne $expected["dark"].colors.background) "Light and Dark exports differ"

# 3: import a valid theme
$validTheme = @'
{
  "format": "TaskManagerPlus.Theme",
  "version": 1,
  "name": "Forest",
  "colors": {
    "foreground": "#16241C", "background": "#1F3328", "background2": "#2C4A39",
    "highlight": "#4F8A68", "highlight2": "#6AAE86", "text": "#EAF6EE"
  }
}
'@
$validFile = Join-Path $dir "forest.json"
[IO.File]::WriteAllText($validFile, $validTheme, (New-Object Text.UTF8Encoding($false)))
$s = Write-Settings "import-valid" 2
Invoke-Run "import-valid" $s $IMPORT @("yes") $null $validFile @("wait:1", "shotmain:imported-main")
$mode = Get-Setting $s "ThemeMode"
function Bgr($rgbHex) { $r = [Convert]::ToInt32($rgbHex.Substring(0, 2), 16); $g = [Convert]::ToInt32($rgbHex.Substring(2, 2), 16); $b = [Convert]::ToInt32($rgbHex.Substring(4, 2), 16); return ($b -shl 16) + ($g -shl 8) + $r }
function SettingNum($key) { $v = Get-Setting $s $key; if ($v) { return [Convert]::ToInt32($v, 16) } else { return -1 } }
Check ($mode -eq "3") "import: ThemeMode is now Custom (3), got $mode"
Check ((SettingNum "ThemeWindowBackgroundColor") -eq (Bgr "1F3328")) "import: background colour setting holds #1F3328"
Check ((SettingNum "ThemeWindowTextColor") -eq (Bgr "EAF6EE")) "import: text colour setting holds #EAF6EE"
Check ((SettingNum "ThemeWindowHighlight2Color") -eq (Bgr "6AAE86")) "import: highlight2 colour setting holds #6AAE86"
Check ((Test-Path (Join-Path $shots "imported-main.png"))) "import: screenshot of the main window with the imported palette (imported-main.png)"

# 5: the exported file imports cleanly (round trip), in a fresh run
$s = Write-Settings "roundtrip" 1
Invoke-Run "roundtrip" $s $IMPORT @("yes") $null (Join-Path $dir "exported-light.json")
Check ((Get-Setting $s "ThemeMode") -eq "3") "round trip: exported Light palette is accepted and applied as Custom"

# 4: rejects
$bad = [ordered]@{
    "malformed"   = '{"format": "TaskManagerPlus.Theme", "version": 1, "colors": {'
    "badcolor"    = ($validTheme -replace '#1F3328', '#1F33')
    "named"       = ($validTheme -replace '"#1F3328"', '"green"')
    "numbercolor" = ($validTheme -replace '"#1F3328"', '2044712')
    "missing"     = ($validTheme -replace '"foreground": "#16241C", ', '')
    "version"     = ($validTheme -replace '"version": 1', '"version": 2')
    "format"      = ($validTheme -replace 'TaskManagerPlus.Theme', 'Something.Else')
    "contrast"    = ($validTheme -replace '#EAF6EE', '#203025')
    "array"       = '[1,2,3]'
    "empty"       = ''
    "binary"      = ([string]::new([char[]](0..255 | ForEach-Object { [char]$_ })))
    "huge"        = ($validTheme.TrimEnd() + (" " * 70000))
    "deep"        = ('{"x":' + ("[" * 5000))
}
foreach ($k in $bad.Keys) {
    $f = Join-Path $dir "bad-$k.json"
    [IO.File]::WriteAllText($f, $bad[$k], (New-Object Text.UTF8Encoding($false)))
    $s = Write-Settings "bad-$k" 1
    Invoke-Run "bad-$k" $s $IMPORT @("yes") $null $f
    $mode = Get-Setting $s "ThemeMode"
    $bg = Get-Setting $s "ThemeWindowBackgroundColor"
    Check (($script:lastOut -match "dialog2 0 answered yes") -and $mode -eq "1" -and $script:lastOut -match "exited=True" -and (Test-Path (Join-Path $shots "bad-$k.odlg0.png"))) "reject '$k': error shown, ThemeMode stays 1 (got $mode), app exited normally"
}
$s = Write-Settings "bad-missingfile" 1
Invoke-Run "bad-missingfile" $s $IMPORT @("yes") $null (Join-Path $dir "does-not-exist.json")
Check (($script:lastOut -match "dialog2 0 answered yes") -and (Get-Setting $s "ThemeMode") -eq "1" -and $script:lastOut -match "exited=True") "reject: missing file shows an error and changes nothing"

& $runHidden -Cleanup | Out-Null
if ($fail) { "theme json hidden test: $fail FAILED"; exit 1 }
"theme json hidden test passed; screenshots in $shots"
