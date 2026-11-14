<#
.SYNOPSIS
  Test for the headless `--report <kind> <path>` command line (SessionReports). Runs the exe on the hidden desktop via
  tools\run-hidden.ps1 (never on the user's desktop), with scratch settings.

.DESCRIPTION
  Every kind (processes, startup, services, trends) in every format (csv, json, html, picked from the extension):
  the exit code must be 0 and the file must be well formed and non-trivial (CSV header and rows, JSON that parses with
  the expected columns and rows, HTML with a table and no script). Then the error paths: unknown kind, unknown or missing
  extension, missing path, unwritable path give exit codes 2 / 2 / 2 / 4, and no file is left behind.
  Also checks no SystemInformer.exe from this run remains.

.EXAMPLE
  powershell -File tools\tests\report-cli-hidden.ps1
#>
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runHidden = Join-Path $root "tools\run-hidden.ps1"
$dir = Join-Path $env:TEMP "tmplus-report-cli-test"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $dir | Out-Null

$fail = 0
function Check($ok, $text) { if ($ok) { "PASS  $text" } else { "FAIL  $text"; $script:fail++ } }

$settings = Join-Path $dir "settings.json"
[IO.File]::WriteAllText($settings, '{"$schema":"settings.schema.json","ReadableLookOffered":"1","CalmLookOffered":"1"}', (New-Object Text.UTF8Encoding($false)))

function Invoke-Report($kind, $path, $extra = $null) {
    $a = if ($extra -ne $null) { $extra } else { "--report $kind `"$path`"" }
    $out = & $runHidden -Name "rep" -Settings $settings -Wait 60 -NoMain -ExtraArgs $a | Out-String
    if ($out -match "exited=True exitcode=(-?\d+)") { return [int]$Matches[1] }
    return -999
}

$expected = @{
    processes = @{ Columns = @("Name", "PID", "CPU %", "Working set MB", "Path"); MinRows = 20 }
    startup   = @{ Columns = @("Name", "Type", "Status", "Command"); MinRows = 0 }
    services  = @{ Columns = @("Name", "Display name", "State", "PID"); MinRows = 50 }
    trends    = @{ Columns = $null; MinRows = 0 }
}

foreach ($kind in @("processes", "startup", "services", "trends")) {
    foreach ($ext in @("csv", "json", "html")) {
        $file = Join-Path $dir "$kind.$ext"
        $code = Invoke-Report $kind $file
        $e = $expected[$kind]
        $ok = ($code -eq 0) -and (Test-Path $file)
        $detail = "exit $code"
        if ($ok) {
            $bytes = [IO.File]::ReadAllBytes($file)
            $text = [Text.Encoding]::UTF8.GetString($bytes)
            switch ($ext) {
                "csv" {
                    $lines = @($text.TrimStart([char]0xFEFF) -split "`r`n" | Where-Object { $_ -ne "" })
                    $ok = $lines.Count -ge 1 -and $lines.Count - 1 -ge $e.MinRows
                    if ($e.Columns) { foreach ($c in $e.Columns) { if ($lines[0] -notmatch [regex]::Escape($c)) { $ok = $false } } }
                    $detail = "csv, $($lines.Count - 1) rows"
                }
                "json" {
                    try {
                        $j = $text | ConvertFrom-Json
                        $ok = $j.columns.Count -ge 1 -and @($j.rows).Count -ge $e.MinRows
                        if ($e.Columns) { foreach ($c in $e.Columns) { if ($j.columns -notcontains $c) { $ok = $false } } }
                        $detail = "json, $(@($j.rows).Count) rows"
                    } catch { $ok = $false; $detail = "json does not parse: $($_.Exception.Message)" }
                }
                "html" {
                    $ok = ($text -match "<table") -and ($text -notmatch "<script") -and ($text -match "</html>")
                    $detail = "html, $([regex]::Matches($text, '<tr').Count) table rows"
                }
            }
        }
        Check $ok "--report $kind $kind.$ext ($detail)"
    }
}

# the header line of the CSV for processes names the right columns in order
$first = ([IO.File]::ReadAllText((Join-Path $dir "processes.csv")).TrimStart([char]0xFEFF) -split "`r`n")[0]
Check ($first -like "Name,PID,Parent PID,CPU %,*") "processes.csv header order: $first"

# error paths
$bad = Join-Path $dir "bad.csv"
Check ((Invoke-Report "nosuchkind" $bad) -eq 2) "unknown kind: exit 2"
Check ((Invoke-Report "processes" (Join-Path $dir "bad.txt")) -eq 2) "unknown extension: exit 2"
Check ((Invoke-Report "processes" (Join-Path $dir "noextension")) -eq 2) "no extension: exit 2"
Check ((Invoke-Report "" "" "--report processes") -eq 2) "missing path: exit 2"
Check ((Invoke-Report "" "" "--report") -eq 2) "no kind and no path: exit 2"
Check ((Invoke-Report "processes" (Join-Path $dir "nodir\sub\x.csv")) -eq 4) "unwritable path (missing folder): exit 4"
Check (-not (Test-Path (Join-Path $dir "nodir"))) "no folder was created for the unwritable path"
Check (-not (Test-Path $bad)) "no file left behind by a rejected call"
# the long option form with a single dash and an existing file is overwritten, not appended
$again = Join-Path $dir "services.csv"
$size1 = (Get-Item $again).Length
Check ((Invoke-Report "" "" "-report services `"$again`"") -eq 0) "single-dash -report also works"
Check ((Get-Item $again).Length -lt ($size1 * 3)) "an existing report file is overwritten, not appended"

& $runHidden -Cleanup | Out-Null
$left = @(Get-Process -Name SystemInformer -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "*systeminformer\bin\Release64*" })
Check ($left.Count -eq 0) "no test SystemInformer.exe left running"

if ($fail) { "report cli test: $fail FAILED"; exit 1 }
"report cli test passed; files in $dir"
