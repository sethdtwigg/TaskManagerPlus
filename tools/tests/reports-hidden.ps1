<#
.SYNOPSIS
  Hidden-desktop test for SessionReports (Phase 7 item 6): real exports of the process list, startup list, services and a
  trends window to CSV, JSON and HTML from the running app, then checks the files. Never touches the user's settings or
  desktop (tools\run-hidden.ps1, scratch settings).

.DESCRIPTION
  Starts a throwaway process whose file name needs escaping (=evil,&a'b.exe: leading '=', comma, ampersand, apostrophe),
  runs the app with scratch settings (TestDirectory set), posts the registered message
  "TaskManagerPlus.SessionReports.Export" (wParam = kind, lParam = format) for all 4 kinds x 3 formats, and checks:
    - all 12 files exist and are non-empty
    - CSV: UTF-8 BOM, header row, the hostile process name is quoted and defused with a leading apostrophe
    - JSON: parses with ConvertFrom-Json, the report/columns/rows shape, rows contain the hostile name verbatim
    - HTML: self-contained (no script, link, img, url(, http), the name appears HTML-escaped (&amp;, &#39;)
    - the process list contains SystemInformer.exe, the services list contains an always-present service (EventLog)
  The file dialog path (System > Export report) is a modal Windows file dialog and is not driven here.

.EXAMPLE
  powershell -File tools\tests\reports-hidden.ps1
#>
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runHidden = Join-Path $root "tools\run-hidden.ps1"
$dir = Join-Path $env:TEMP "tmplus-reports-test"
$out = Join-Path $dir "out"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $out | Out-Null

$fail = 0
function Check($ok, $text) { if ($ok) { "PASS  $text" } else { "FAIL  $text"; $script:fail++ } }

$hostile = "=evil,&a'b.exe"
$tmp = Join-Path $dir "target"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$exe = Join-Path $tmp $hostile
Copy-Item (Join-Path $env:SystemRoot "System32\PING.EXE") $exe
$target = Start-Process -FilePath $exe -ArgumentList "-n 900 127.0.0.1" -WindowStyle Hidden -PassThru

$settings = Join-Path $dir "settings.json"
$h = @{
    '$schema' = "settings.schema.json"; "EnableThemeSupport" = "1"; "ThemeMode" = "2"; "ReadableLookOffered" = "1"; "CalmLookOffered" = "1"
    "TaskManagerPlus.SessionReports.TestDirectory" = $out
    "TaskManagerPlus.ResourceTrends.SampleSeconds" = "1"
}
[IO.File]::WriteAllText($settings, ($h | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))

$steps = @("wait:12")
foreach ($k in 0..3) { foreach ($f in 0..2) { $steps += "msg:TaskManagerPlus.SessionReports.Export,$k,$f"; $steps += "wait:1" } }
$steps += "wait:2"

try {
    $log = & $runHidden -Name reports -Settings $settings -Wait 9 -Wait2 1 -Steps $steps -ExitCmd | Out-String
    $log.Trim() -split "`r?`n" | Where-Object { $_ -match "posted|exited" } | Select-Object -First 3 | ForEach-Object { "   $_" }
}
finally {
    if ($target -and -not $target.HasExited) { Stop-Process -Id $target.Id -Force }
    & $runHidden -Cleanup | Out-Null
}

$kinds = @("processes", "startup", "services", "trends")
$exts = @("csv", "json", "html")
foreach ($k in $kinds) { foreach ($e in $exts) {
    $p = Join-Path $out "report-$k.$e"
    Check ((Test-Path $p) -and (Get-Item $p).Length -gt 50) "report-$k.$e written ($(if (Test-Path $p) { (Get-Item $p).Length } else { 0 }) bytes)"
} }

# CSV
$csvPath = Join-Path $out "report-processes.csv"
if (Test-Path $csvPath) {
    $b = [IO.File]::ReadAllBytes($csvPath)
    Check ($b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) "CSV starts with a UTF-8 BOM"
    $text = [Text.Encoding]::UTF8.GetString($b)
    Check ($text.TrimStart([char]0xFEFF).StartsWith("Name,PID,Parent PID,CPU %")) "CSV header row"
    Check ($text.Contains("`"'=evil,&a'b.exe`"")) "CSV: hostile name quoted and defused with a leading apostrophe"
    $rows = $text | ConvertFrom-Csv
    Check (@($rows | Where-Object { $_.Name -eq "SystemInformer.exe" }).Count -ge 1) "CSV parses back and lists SystemInformer.exe"
    Check (@($rows | Where-Object { $_.Name -eq "'$hostile" }).Count -eq 1) "CSV parses back to the hostile name (with the guard apostrophe) as one cell"
}
# JSON
$jsonPath = Join-Path $out "report-processes.json"
if (Test-Path $jsonPath) {
    $j = Get-Content $jsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
    Check ($j.report -eq "Process list" -and $j.columns.Count -eq 13) "JSON shape: report title and 13 columns"
    Check (@($j.rows | Where-Object { $_.Name -eq $hostile }).Count -eq 1) "JSON keeps the hostile name verbatim"
}
$j = Get-Content (Join-Path $out "report-services.json") -Raw -Encoding UTF8 | ConvertFrom-Json
Check (@($j.rows | Where-Object { $_.Name -eq "EventLog" }).Count -eq 1) "services JSON lists EventLog"
$j = Get-Content (Join-Path $out "report-trends.json") -Raw -Encoding UTF8 | ConvertFrom-Json
Check ($j.columns[0] -eq "Process" -and $j.rows.Count -ge 1) "trends JSON has recorded processes ($($j.rows.Count) rows)"
$j = Get-Content (Join-Path $out "report-startup.json") -Raw -Encoding UTF8 | ConvertFrom-Json
Check ($j.columns -contains "Impact" -and $j.columns -contains "Boot delay (ms)") "startup JSON has Impact and Boot delay columns ($($j.rows.Count) rows)"
# HTML
foreach ($k in $kinds) {
    $hp = Join-Path $out "report-$k.html"
    if (-not (Test-Path $hp)) { continue }
    $t = Get-Content $hp -Raw -Encoding UTF8
    $head = $t.Substring(0, $t.IndexOf("<body>"))
    # data cells may legitimately contain text such as a command line with "http://": it must be escaped, so no tag can come from it
    Check (($t -notmatch '<script|<link|<img|<iframe|<a |<object|<embed|<style[^>]*src') -and ($head -notmatch 'url\(|@import|https?://|\ssrc=|\shref=')) "HTML ($k) is self-contained (no tags from data, no external references in the head)"
    Check ($t.StartsWith("<!DOCTYPE html>") -and $t.TrimEnd().EndsWith("</html>")) "HTML ($k) is a complete document"
}
$t = Get-Content (Join-Path $out "report-processes.html") -Raw -Encoding UTF8
Check ($t.Contains("=evil,&amp;a&#39;b.exe")) "HTML escapes the hostile name (&amp; and &#39;)"

# palette: the export commands are listed (screenshot only; Enter would open a modal file dialog)
$shots = Join-Path $dir "shots"
New-Item -ItemType Directory -Force -Path $shots | Out-Null
$o = & $runHidden -Name reports-palette -Settings $settings -Wait 9 -Wait2 1 -Width 1100 -Height 800 -Steps @("msg:TaskManagerPlus.QuickActions.Open", "wait:1", "type:export proc", "wait:1", "shot:palette-export") | Out-String
if ($o -match "run folder: (.+)") { Get-ChildItem $Matches[1].Trim() -Filter *.png | ForEach-Object { Copy-Item $_.FullName (Join-Path $shots $_.Name) -Force } }
Check (($o -notmatch "step (shot|type): no ") -and (Test-Path (Join-Path $shots "palette-export.png"))) "palette opened and 'export proc' typed (screenshot $shots\palette-export.png)"
& $runHidden -Cleanup | Out-Null

"files: $out"
if ($fail) { "reports test FAILED ($fail)"; exit 1 }
"reports test passed"
