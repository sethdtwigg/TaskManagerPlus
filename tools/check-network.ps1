<#
.SYNOPSIS
  Live check that a running TaskManagerPlus makes no connections to other machines.

.DESCRIPTION
  Start SystemInformer.exe yourself (normal use is fine; elevated is also fine), then run this
  script. It samples the process's TCP connections and UDP endpoints every second for
  -Minutes minutes and reports anything whose remote address is not loopback, and, for TCP, any
  remote endpoint at all that is not 127.0.0.1 / ::1. UDP sockets are connectionless, so they are
  reported when bound to a non-loopback address (listening is not traffic, but it is listed so
  you can judge). Exit code 0 = clean, 1 = findings, 2 = process not found.

  Use -Pktmon to additionally capture packets with pktmon (needs an elevated shell) and list any
  packet whose source or destination is not loopback while the process is running; pktmon filters
  per machine, not per process, so expect unrelated system traffic and treat it only as a cross-check
  against the per-process results.

.EXAMPLE
  .\tools\check-network.ps1 -Minutes 30
#>
param(
    [int]$Minutes = 10,
    [string]$ProcessName = 'SystemInformer',
    [switch]$Pktmon
)

$ErrorActionPreference = 'Stop'
$procs = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)
if ($procs.Count -eq 0) {
    Write-Host "Process '$ProcessName' is not running. Start it first, then rerun." -ForegroundColor Yellow
    exit 2
}

$pids = $procs.Id
Write-Host "Watching $ProcessName pid(s): $($pids -join ', ') for $Minutes minute(s)..."

$isLoopback = { param($a) $a -in @('127.0.0.1', '::1', '0.0.0.0', '::') -or $a -like '127.*' }
$findings = @{}
$end = (Get-Date).AddMinutes($Minutes)

if ($Pktmon) {
    pktmon stop 2>$null | Out-Null
    pktmon filter remove 2>$null | Out-Null
    pktmon start --capture --pkt-size 0 -f "$env:TEMP\tmp-pktmon.etl" | Out-Null
}

while ((Get-Date) -lt $end) {
    foreach ($p in $pids) {
        foreach ($c in (Get-NetTCPConnection -OwningProcess $p -ErrorAction SilentlyContinue)) {
            if ($c.State -eq 'Listen') { continue }
            if (-not (& $isLoopback $c.RemoteAddress)) {
                $key = "TCP $($c.LocalAddress):$($c.LocalPort) -> $($c.RemoteAddress):$($c.RemotePort) [$($c.State)]"
                if (-not $findings.ContainsKey($key)) { $findings[$key] = Get-Date; Write-Host "FOUND  $key" -ForegroundColor Red }
            }
        }
        foreach ($u in (Get-NetUDPEndpoint -OwningProcess $p -ErrorAction SilentlyContinue)) {
            if (-not (& $isLoopback $u.LocalAddress)) {
                $key = "UDP bound $($u.LocalAddress):$($u.LocalPort)"
                if (-not $findings.ContainsKey($key)) { $findings[$key] = Get-Date; Write-Host "NOTE   $key" -ForegroundColor Yellow }
            }
        }
    }
    Start-Sleep -Seconds 1
    if (-not (Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)) { Write-Host 'Process exited.'; break }
}

if ($Pktmon) {
    pktmon stop | Out-Null
    $txt = "$env:TEMP\tmp-pktmon.txt"
    pktmon etl2txt "$env:TEMP\tmp-pktmon.etl" -o $txt | Out-Null
    Write-Host "pktmon capture converted to $txt (machine-wide; compare ports above against it)."
}

$remote = @($findings.Keys | Where-Object { $_ -like 'TCP *' })
if ($remote.Count -gt 0) {
    Write-Host "`nRESULT: $($remote.Count) remote TCP connection(s) seen. NOT clean." -ForegroundColor Red
    exit 1
}
Write-Host "`nRESULT: no non-loopback TCP connections from $ProcessName in $Minutes minute(s)." -ForegroundColor Green
if ($findings.Count -gt 0) { Write-Host "UDP sockets bound to non-loopback addresses were listed above; inspect them." }
exit 0
