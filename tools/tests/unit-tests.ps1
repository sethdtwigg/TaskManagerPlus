<#
.SYNOPSIS
  Builds and runs the pure-C unit tests: fuzzy matcher, trend file format, thinning, aggregation, leak detection
  (unit-tests.c), report CSV/JSON/HTML escaping (test-reports.c), boot event-log parsing and impact rating
  (test-boot.c), profile apply/revert and JSON round trip (test-profiles.c), efficiency advisor ranking (test-advisor.c), rules share code encode/decode (test-sharecode.c).
  Needs only cl.exe from Visual Studio; no System Informer headers, no app, no elevation.
#>
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$out = Join-Path $env:TEMP "tmplus-unit"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$plug = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) "plugins"
$vcvars = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat" | Sort-Object FullName | Select-Object -Last 1
if (-not $vcvars) { throw "vcvars64.bat not found" }
$env:PATH = "C:\Program Files (x86)\Microsoft Visual Studio\Installer;" + $env:PATH   # vcvars calls vswhere.exe

# test program -> pure-C sources it links (relative to plugins\)
$suites = [ordered]@{
    "unit-tests" = @("QuickActions\fuzzy.c", "ResourceTrends\leak.c", "ResourceTrends\trendfmt.c")
    "test-reports" = @("SessionReports\reportfmt.c")
    "test-boot" = @("StartupManager\bootparse.c")
    "test-profiles" = @("Profiles\profilecore.c")
    "test-advisor" = @("EfficiencyAdvisor\advisor.c")
    "test-sharecode" = @("ProcessRules\sharecode.c")
    "test-cliparse" = @("SessionReports\cliparse.c")
    "test-tmrepair" = @("..\SystemInformer\tmrepair.c")
}

$failed = 0
foreach ($name in $suites.Keys) {
    $test = Join-Path $here "unit\$name.c"
    if (-not (Test-Path $test)) { continue }
    $missing = $suites[$name] | Where-Object { -not (Test-Path (Join-Path $plug $_)) }
    if ($missing) { "$name : skipped (missing $($missing -join ', '))"; continue }
    $src = "`"$test`" " + (($suites[$name] | ForEach-Object { "`"$plug\$_`"" }) -join " ")
    $exe = Join-Path $out "$name.exe"
    $obj = Join-Path $out "obj-$name"
    New-Item -ItemType Directory -Force -Path $obj | Out-Null
    $cmd = "`"$($vcvars.FullName)`" >nul && cl /nologo /W4 /WX /std:c11 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Fe$exe /Fo$obj\ $src && $exe"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { "$name FAILED"; $failed++ }
}
if ($failed) { "unit tests FAILED ($failed suite(s))"; exit 1 }
"unit tests passed"
