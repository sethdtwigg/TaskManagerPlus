<#
.SYNOPSIS
    Regression guard for the TaskManagerPlus offline-only rule (Phase 6a).

.DESCRIPTION
    Fails (exit 1) when tracked source contains an outbound-network API, an
    upstream/remote host, or an unexpected URL. With -Binaries it also checks the
    built exe/DLLs in bin\Release64 for network DLL imports and remote strings.

    Run from anywhere:  powershell -File tools\check-offline.ps1 [-Binaries]

    Provenance in code comments is ignored for the URL rule (lines whose first
    non-blank characters are //, /*, * or #). Vendored code (packages, phnt,
    tools/thirdparty) is not scanned. Intentional exceptions live in $Allow below, each with a reason.
#>
param([switch]$Binaries)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$exclude = @(':(exclude)packages', ':(exclude)phnt', ':(exclude)tools/thirdparty',
             ':(exclude)tools/check-offline.ps1',
             ':(exclude)*.mmdb', ':(exclude)*.png', ':(exclude)*.ico',
             ':(exclude)*.bin', ':(exclude)*.zip', ':(exclude)*.lib', ':(exclude)*.dll')

# Network APIs / libraries (anywhere in tracked text files).
$apiRules = @(
    'WinHttp\w*', 'InternetOpen\w*', 'InternetConnect\w*', 'URLDownloadToFile\w*', 'WSAStartup',
    'getaddrinfo', 'GetAddrInfo\w*', 'DnsQuery_W', 'DnsQueryEx', 'IcmpCreateFile', 'IcmpSendEcho\w*',
    'Icmp6SendEcho\w*', 'PhHttp\w*', 'PhDnsQuery\w*', '\bwinhttp\b', '\bwininet\b', '\burlmon\b',
    '\bws2_32\b', 'PhSearchOnlineString', 'EnableNetworkResolve', '_NT_SYMBOL_PATH', '_NT_ALT_SYMBOL_PATH'
)
# Remote hosts / services.
$hostRules = @(
    'winsiderss', 'systeminformer\.(io|com)', 'system-informer\.com', 'sourceforge', 'github\.com',
    'githubusercontent', 'virustotal\.com', 'hybrid-analysis', 'jotti', 'filescan\.io', 'maxmind\.com',
    'msdl\.microsoft', 'cloudflare-dns', 'duckduckgo', 'catalog\.update\.microsoft', 'whois\.(iana|arin)',
    'opencode\.ai', 'discord\.com', 'shields\.io'
)
# Any other absolute URL in source/resources (comments ignored).
$urlRule = 'https?://'

# path-regex => (line-regex, reason). A hit is allowed only if both match.
$Allow = @(
    @{ Path = '^SystemInformer/main\.c$'; Line = 'migrate-old-default'; Why = 'migrates the old default symbol path to the local-only default' },
    @{ Path = '\.svg$|\.filters$|\.pubxml$|^tools/icon/[^/]+\.py$'; Line = 'www\.w3\.org|schemas\.microsoft\.com'; Why = 'XML namespaces in resources/project files, never fetched' },
    @{ Path = '^SystemInformer/resources/etwguids\.txt$'; Line = '.'; Why = 'local ETW provider GUID/name table' },
    @{ Path = '^tools/peview/hashprp\.c$'; Line = 'ws2_32\.dll'; Why = 'file name of a system DLL parsed for export ordinals, not loaded' },
    @{ Path = '^tools/tests/phlib-test/t_util\.c$'; Line = 'python\.org'; Why = 'namespace string hashed into a test GUID, not contacted' },
    @{ Path = '\.manifest$|\.rc$|\.natvis$|\.vcxproj|\.props$|\.targets$|\.sln|\.xml$|\.csproj$'; Line = 'schemas\.microsoft\.com|schemas\.openxmlformats|www\.w3\.org|msbuild/2003|aka\.ms/'; Why = 'XML namespaces / schema identifiers, never fetched' },
    @{ Path = '^tools/CustomBuildTool/NativeMethods\.json$'; Line = 'aka\.ms/'; Why = 'CsWin32 schema identifier, never fetched' },
    @{ Path = '^SystemInformer/resources/settings\.schema\.json$'; Line = 'json-schema\.org'; Why = 'JSON Schema meta-schema identifier, never fetched' },
    @{ Path = '^tools/tests/(unit/test-boot\.c|boot-hidden\.ps1)$'; Line = 'schemas\.microsoft\.com/win/2004/08/events/event'; Why = 'XML namespace of the sample Windows event records the boot parser is tested with, never fetched' },
    @{ Path = '^tools/tests/unit/test-reports\.c$'; Line = 'Has\(s, "https?://"\)'; Why = 'negative assertions: a generated HTML report must not contain any URL' },
    @{ Path = '^plugins/ExtendedTools/Efi/'; Line = 'opensource\.org'; Why = 'licence header text' },
    @{ Path = '^(LICENSE|COPYRIGHT)\.txt$'; Line = '.'; Why = 'licence text' },
    @{ Path = '^README\.md$'; Line = 'LICENSE\.txt'; Why = 'local link' },
    @{ Path = '^\.github/workflows/ci\.yml$'; Line = 'api\.nuget\.org/v3/index\.json'; Why = 'CI-only: the runner restores build packages from nuget.org into a throwaway workspace copy of nuget.config; the shipped app and the repo nuget.config stay offline' },
    @{ Path = '^README\.md$'; Line = 'github\.com/sethdtwigg/TaskManagerPlus'; Why = 'this project''s own repository name as plain text, never fetched' }
)

function Test-Allowed($path, $text) {
    foreach ($a in $Allow) {
        if ($path -match $a.Path -and $text -match $a.Line) { return $true }
    }
    return $false
}

$hits = New-Object System.Collections.Generic.List[string]

function Scan($pattern, [bool]$skipComments) {
    $lines = git grep -n -I -i -E $pattern -- . @exclude 2>$null
    foreach ($l in $lines) {
        if ($l -notmatch '^([^:]+):(\d+):(.*)$') { continue }
        $p = $Matches[1]; $n = $Matches[2]; $t = $Matches[3]
        if ($skipComments -and $t -match '^\s*(//|/\*|\*|#)') { continue }
        if (Test-Allowed $p $t) { continue }
        $hits.Add(('{0}:{1}: {2}' -f $p, $n, $t.Trim()))
    }
}

Scan ('(' + ($apiRules -join '|') + ')') $false
Scan ('(' + ($hostRules -join '|') + ')') $true
Scan $urlRule $true

# Top-level docs must not name or point at the upstream project except for the plain-text credit
# (README.md, NOTICE.txt). "Process Hacker" / author handles are never allowed there.
$docFiles = @('README.md', 'README.txt', 'NOTICE.txt', 'CHANGELOG.txt', 'HACKING.md')
$docLines = git grep -n -I -i -E 'process ?hacker|processhacker|wj32|dmex' -- @docFiles 2>$null
foreach ($l in $docLines) { $hits.Add('doc: ' + $l) }
$docLines = git grep -n -I -i -E 'system ?informer' -- README.txt 2>$null
foreach ($l in $docLines) { if ($l -notmatch 'SystemInformer\.exe') { $hits.Add('doc: ' + $l) } }

if ($Binaries) {
    $dumpbin = Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName | Select-Object -Last 1
    $files = @(Get-ChildItem bin\Release64\*.exe, bin\Release64\plugins\*.dll -ErrorAction SilentlyContinue)
    $netDll = 'winhttp|wininet|urlmon|ws2_32|wsock32|dnsapi|wldap32|mswsock|httpapi|IcmpCreateFile|IcmpSendEcho|Icmp6SendEcho'
    $strRule = [regex]'(?i)(https?://(?!schemas\.microsoft\.com)[\x21-\x7e]{3,}|winsiderss|systeminformer\.(io|com)|msdl\.microsoft|github\.com|sourceforge|virustotal\.com|maxmind\.com)'
    foreach ($f in $files) {
        if ($dumpbin) {
            $imp = & $dumpbin.FullName -imports -nologo $f.FullName 2>&1 | Select-String -Pattern $netDll
            foreach ($i in $imp) { $hits.Add(('{0}: import {1}' -f $f.Name, $i.Line.Trim())) }
        }
        $bytes = [IO.File]::ReadAllBytes($f.FullName)
        $ascii = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
        $wide = [Text.Encoding]::Unicode.GetString($bytes)
        foreach ($s in @($ascii, $wide)) {
            foreach ($m in $strRule.Matches($s)) {
                # The symbol-path migration literal in SystemInformer.exe is intentional.
                if ($f.Name -eq 'SystemInformer.exe' -and $m.Value -match 'msdl\.microsoft|^https://msdl') { continue }
                $hits.Add(('{0}: string {1}' -f $f.Name, $m.Value))
            }
        }
    }
}

if ($hits.Count) {
    $hits | Sort-Object -Unique | ForEach-Object { Write-Host $_ }
    Write-Host ("`nFAIL: {0} offline-rule hit(s)." -f ($hits | Sort-Object -Unique).Count) -ForegroundColor Red
    exit 1
}
Write-Host 'OK: no outbound-network code, remote hosts or unexpected URLs found.' -ForegroundColor Green
exit 0
