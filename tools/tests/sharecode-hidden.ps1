<#
.SYNOPSIS
  Hidden-desktop test for the rules share code (small gap of Phase 7): the real plugin exports the share code of its rules
  and imports it into a second, independent set of settings. Never touches the user's settings or desktop.

.DESCRIPTION
  The clipboard cannot be driven on a hidden desktop, so the plugin has a test seam (registered message
  TaskManagerPlus.ProcessRules.Command, setting TestShareFile): op 1 writes the code to the file, op 2 imports the code in
  that file and writes "OK <n>" / "FAIL <reason>" to <file>.result. The clipboard menu items use the same two functions.
  Run A (rules s1 on/below normal/efficiency, s2 off) exports. The file starts with TMPR1:, decodes (base64) to JSON with
  both rules and a CRC-32 that matches (computed here independently).
  Run B (own rules: s1 disabled, s3) imports it: result OK 2, s1 replaced, s2 added, s3 untouched.
  Run C imports a code with one character changed and one that is cut off: FAIL, rules unchanged.

.EXAMPLE
  powershell -File tools\tests\sharecode-hidden.ps1
#>
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runHidden = Join-Path $root "tools\run-hidden.ps1"
$dir = Join-Path $env:TEMP "tmplus-sharecode-test"
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
$a = Join-Path $dir "A"; $b = Join-Path $dir "B"
New-Item -ItemType Directory -Force -Path $a, $b | Out-Null

$fail = 0
function Check($ok, $text) { if ($ok) { "PASS  $text" } else { "FAIL  $text"; $script:fail++ } }
function Write-Json($path, $obj) { [IO.File]::WriteAllText($path, ($obj | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false))) }
function Rule($program, $enabled, $priority, $eff) {
    @{ id = 0; enabled = $enabled; muted = $false; program = $program; path = ""; startPriority = $priority; startAffinity = 0; startAffinityMask = 0; startEfficiency = $eff; startEnd = $false; command = ""
       watchdogs = @(1..4 | ForEach-Object { @{ enabled = $false; metric = 0; threshold = 0; duration = 0; action = 0 } }) }
}
function Run($folder, $name, $op) {
    $settings = Join-Path $folder "settings.json"
    Write-Json $settings @{ '$schema' = "settings.schema.json"; "EnableThemeSupport" = "1"; "ThemeMode" = "2"; "ReadableLookOffered" = "1"; "CalmLookOffered" = "1"
        "TaskManagerPlus.ProcessRules.TestShareFile" = $script:codeFile }
    & $runHidden -Name $name -Settings $settings -Wait 9 -Wait2 1 -Steps @("wait:2", "msg:TaskManagerPlus.ProcessRules.Command,$op", "wait:2") -ExitCmd | Out-Null
}
function Rules($folder) { (Get-Content (Join-Path $folder "tmp-rules.json") -Raw -Encoding UTF8 | ConvertFrom-Json).rules }

# independent CRC-32 (IEEE) for the check
function Crc32([byte[]]$bytes) {
    $crc = [uint32]::MaxValue
    foreach ($byte in $bytes) {
        $crc = $crc -bxor $byte
        for ($i = 0; $i -lt 8; $i++) { if ($crc -band 1) { $crc = ($crc -shr 1) -bxor 0xEDB88320 } else { $crc = $crc -shr 1 } }
    }
    return (-bnot $crc) -band [uint32]::MaxValue
}

$script:codeFile = Join-Path $dir "code.txt"
try {
    Write-Json (Join-Path $a "tmp-rules.json") @{ version = 1; rules = @((Rule "tmp-s1.exe" $true 5 $true), (Rule "tmp-s2.exe" $false 0 $false)) }
    Run $a "share-export" 1
    Check (Test-Path $codeFile) "run A wrote a share code file"
    $code = (Get-Content $codeFile -Raw).Trim()
    Check ($code.StartsWith("TMPR1:") -and $code -match '^TMPR1:[A-Za-z0-9+/]+=*\.[0-9a-f]{8}$') "code has the TMPR1: prefix, base64 payload and 8 hex digit checksum ($($code.Length) characters)"
    $b64, $crc = $code.Substring(6) -split "\."
    $bytes = [Convert]::FromBase64String($b64)
    $json = [Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
    Check (@($json.rules).Count -eq 2 -and ($json.rules | Where-Object { $_.program -eq "tmp-s1.exe" }).startPriority -eq 5) "payload decodes to the rules JSON (2 rules, s1 priority 5)"
    Check (('{0:x8}' -f (Crc32 $bytes)) -eq $crc) "checksum matches an independent CRC-32 of the payload"

    # run B: import into a different rule set
    Write-Json (Join-Path $b "tmp-rules.json") @{ version = 1; rules = @((Rule "tmp-s1.exe" $false 0 $false), (Rule "tmp-s3.exe" $true 1 $false)) }
    Run $b "share-import" 2
    $result = (Get-Content "$codeFile.result" -Raw).Trim()
    Check ($result -eq "OK 2") "run B import result: $result"
    $r = Rules $b
    $s1 = $r | Where-Object { $_.program -eq "tmp-s1.exe" }; $s2 = $r | Where-Object { $_.program -eq "tmp-s2.exe" }; $s3 = $r | Where-Object { $_.program -eq "tmp-s3.exe" }
    Check (@($r).Count -eq 3) "three rules after the import (s1 replaced, s2 added, s3 kept)"
    Check ($s1.enabled -and $s1.startPriority -eq 5 -and $s1.startEfficiency) "s1 was replaced by the imported rule (enabled, below normal, efficiency mode)"
    Check ($s2 -and -not $s2.enabled) "s2 was added (disabled as in the code)"
    Check ($s3.enabled -and $s3.startPriority -eq 1) "s3 untouched"

    # run C: damaged codes change nothing
    function Summary($folder) { (Rules $folder | Sort-Object program | ForEach-Object { "$($_.program)|$($_.enabled)|$($_.startPriority)|$($_.startEfficiency)" }) -join ";" }  # ids are renumbered on every load, so compare the content
    $before = Summary $b
    $chars = $code.ToCharArray(); $chars[12] = if ($chars[12] -eq 'A') { 'B' } else { 'A' }
    [IO.File]::WriteAllText($codeFile, (-join $chars), (New-Object Text.UTF8Encoding($false)))
    Run $b "share-damaged" 2
    $result = (Get-Content "$codeFile.result" -Raw).Trim()
    Check ($result -like "FAIL*checksum*") "a code with one changed character is rejected ($result)"
    [IO.File]::WriteAllText($codeFile, $code.Substring(0, $code.Length - 20), (New-Object Text.UTF8Encoding($false)))
    Run $b "share-cut" 2
    $result = (Get-Content "$codeFile.result" -Raw).Trim()
    Check ($result -like "FAIL*") "a cut-off code is rejected ($result)"
    Check ((Summary $b) -eq $before) "rules file unchanged after the rejected codes"
}
finally {
    & $runHidden -Cleanup | Out-Null
}

if ($fail) { "share code test FAILED ($fail)"; exit 1 }
"share code test passed"
