<#
.SYNOPSIS
  Builds the local NuGet feed (..\offline-packages) that nuget.config points at, from packages this machine already has.

.DESCRIPTION
  TaskManagerPlus builds without any remote NuGet source. Two sets of packages are needed:
    * packages.config (Windows SDK / WDK for C++), extracted under .\packages by "nuget restore", each folder holding its .nupkg;
    * the .NET packages of tools\CustomBuildTool (CsWin32, SharpCompress, System.CommandLine, runtime packs, ILCompiler),
      restored into the global packages folder (%USERPROFILE%\.nuget\packages) and listed in obj\project.assets.json.
  This script copies the .nupkg files of both sets into one flat folder. It never touches the network. Run it once on a
  machine that has already restored (a machine that built the project online, or an offline cache copied from one).
  The feed is not part of git: the Windows SDK/WDK packages are ~0.5 GB and under Microsoft's licence terms.

.PARAMETER Feed
  Destination folder (default: ..\offline-packages next to the repository).
#>
param([string]$Feed = (Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) 'offline-packages'))

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$global = if ($env:NUGET_PACKAGES) { $env:NUGET_PACKAGES } else { Join-Path $env:USERPROFILE '.nuget\packages' }
New-Item -ItemType Directory -Force -Path $Feed | Out-Null
$copied = 0; $missing = @()

function Add-Package($id, $version, $source) {
    $name = "$id.$version.nupkg".ToLowerInvariant()
    $dest = Join-Path $Feed $name
    if (Test-Path $source) { if (-not (Test-Path $dest)) { Copy-Item $source $dest; $script:copied++ } }
    else { $script:missing += "$id $version" }
}

# 1. packages.config
([xml](Get-Content (Join-Path $root 'packages.config'))).packages.package | ForEach-Object {
    $dir = Join-Path $root "packages\$($_.id).$($_.version)"
    Add-Package $_.id $_.version (Join-Path $dir "$($_.id).$($_.version).nupkg")
}

# 2. CustomBuildTool: libraries and download dependencies from the restore result
$assets = Join-Path $root 'tools\CustomBuildTool\obj\project.assets.json'
if (-not (Test-Path $assets)) { throw "Run 'dotnet restore tools\CustomBuildTool\CustomBuildTool.csproj' first (needs the packages once)." }
$json = Get-Content $assets -Raw | ConvertFrom-Json
$entries = @()
$json.libraries.PSObject.Properties | Where-Object { $_.Value.type -eq 'package' } | ForEach-Object { $entries += ,($_.Name -split '/') }
$json.project.frameworks.PSObject.Properties | ForEach-Object {
    if ($_.Value.downloadDependencies) { $_.Value.downloadDependencies | ForEach-Object { $entries += ,@($_.name, ($_.version -replace '^[\[\(]([^,\]\)]+).*$', '$1')) } }
}
foreach ($e in $entries) {
    $id = $e[0].ToLowerInvariant(); $ver = $e[1].ToLowerInvariant()
    Add-Package $id $ver (Join-Path $global "$id\$ver\$id.$ver.nupkg")
}

# 3. build_init restores CustomBuildTool for win-x86 / win-x64 / win-arm64 with PublishAot, which the default restore above does not list:
#    copy the NativeAOT runtime packs that the global folder has for those RIDs (a plain restore needs network once to get them).
foreach ($rid in 'win-x86', 'win-x64', 'win-arm64') {
    $id = "microsoft.netcore.app.runtime.nativeaot.$rid"
    $dir = Join-Path $global $id
    if (Test-Path $dir) {
        Get-ChildItem $dir -Directory | ForEach-Object { Add-Package $id $_.Name (Join-Path $_.FullName "$id.$($_.Name).nupkg") }
    } else { $missing += "$id (only needed to build for $rid; ignore for x64/x86)" }
}

$size = (Get-ChildItem $Feed -Filter *.nupkg | Measure-Object Length -Sum).Sum / 1MB
"Feed: $Feed  ($((Get-ChildItem $Feed -Filter *.nupkg).Count) packages, $([math]::Round($size)) MB, $copied copied now)"
if ($missing) { "MISSING from the local caches (restore them online once): " + ($missing -join ', ') }
