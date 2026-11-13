<#
.SYNOPSIS
  Builds and runs the pure-C unit tests (fuzzy matcher, trend file format, thinning, aggregation, leak detection).
  Needs only cl.exe from Visual Studio; no System Informer headers, no app, no elevation.
#>
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$out = Join-Path $env:TEMP "tmplus-unit"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$plug = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) "plugins"
$vcvars = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat" | Sort-Object FullName | Select-Object -Last 1
if (-not $vcvars) { throw "vcvars64.bat not found" }
$src = "`"$here\unit\unit-tests.c`" `"$plug\QuickActions\fuzzy.c`" `"$plug\ResourceTrends\leak.c`" `"$plug\ResourceTrends\trendfmt.c`""
$env:PATH = "C:\Program Files (x86)\Microsoft Visual Studio\Installer;" + $env:PATH   # vcvars calls vswhere.exe
$cmd = "`"$($vcvars.FullName)`" >nul && cl /nologo /W4 /WX /std:c11 /Fe$out\unit-tests.exe /Fo$out\ $src && $out\unit-tests.exe"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { "unit tests FAILED"; exit 1 }
"unit tests passed"
