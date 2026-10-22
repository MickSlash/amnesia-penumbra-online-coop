# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tells apart where co-op latency spikes come from. Run it on BOTH PCs at the
# same time, during a play session, then compare the two summaries:
#
#   - the partner (VPN address) and the internet reference spike together
#     -> this PC's own internet line (Wi-Fi, saturated upload, ISP);
#   - only the partner spikes, on both PCs at the same moments
#     -> the VPN path (e.g. a Radmin relay) or the partner's line;
#   - nothing spikes here while the game reports gaps
#     -> look at the game/PC itself (stalls), not the network.
#
# Usage (Windows PowerShell, no admin rights needed):
#   ./NetworkProbe.ps1 -Partner 26.x.x.x            # until Ctrl+C
#   ./NetworkProbe.ps1 -Partner 26.x.x.x -Minutes 30
# Output: a CSV with one row per probe and a summary of spikes.
param(
    [Parameter(Mandatory=$true)][string]$Partner,
    # Any stable, always-on internet host works as reference.
    [string]$Reference = '1.1.1.1',
    [double]$Minutes = 0,
    [int]$IntervalMs = 200,
    [int]$SpikeMs = 150,
    [string]$OutFile = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) ("coop-network-probe-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".csv"))
)
$ErrorActionPreference = 'Stop'
$targets = @($Partner, $Reference)
$pingers = @{}
foreach($t in $targets) { $pingers[$t] = New-Object System.Net.NetworkInformation.Ping }
$samples = @{}
foreach($t in $targets) { $samples[$t] = New-Object System.Collections.Generic.List[object] }
"time,target,rtt_ms,status" | Set-Content -LiteralPath $OutFile -Encoding UTF8
$deadline = if($Minutes -gt 0) { (Get-Date).AddMinutes($Minutes) } else { [DateTime]::MaxValue }
Write-Output "Probing partner $Partner and reference $Reference every $IntervalMs ms. Log: $OutFile"
Write-Output "Press Ctrl+C to stop (the summary is printed on exit)."

function Write-Summary {
    Write-Output ""
    Write-Output "=== Summary ($((Get-Date).ToString('HH:mm:ss'))) ==="
    foreach($t in $targets) {
        $all = $samples[$t]
        if($all.Count -eq 0) { continue }
        $ok = @($all | Where-Object { $_.Status -eq 'Success' } | ForEach-Object { $_.Rtt } | Sort-Object)
        $lost = $all.Count - $ok.Count
        $label = if($t -eq $Partner) { 'partner  ' } else { 'reference' }
        if($ok.Count -eq 0) { Write-Output "$label $t : all $($all.Count) probes lost"; continue }
        $p50 = $ok[[int][Math]::Floor(($ok.Count - 1) * 0.5)]
        $p95 = $ok[[int][Math]::Floor(($ok.Count - 1) * 0.95)]
        $spikes = @($all | Where-Object { $_.Status -ne 'Success' -or $_.Rtt -ge $SpikeMs })
        Write-Output ("{0} {1,-15} probes={2} lost={3} ({4:N1}%) median={5}ms p95={6}ms max={7}ms spikes>={8}ms: {9}" -f
            $label, $t, $all.Count, $lost, (100.0 * $lost / $all.Count), $p50, $p95, $ok[-1], $SpikeMs, $spikes.Count)
        # Merge consecutive spiky probes into episodes, so a 3 s stall reads as one event.
        $episodes = @(); $start = $null; $last = $null; $worst = 0
        foreach($s in $all) {
            $bad = $s.Status -ne 'Success' -or $s.Rtt -ge $SpikeMs
            if($bad) {
                if(-not $start) { $start = $s.Time; $worst = 0 }
                $last = $s.Time
                $worst = [Math]::Max($worst, $(if($s.Status -eq 'Success') { $s.Rtt } else { 9999 }))
            } elseif($start) {
                $episodes += [pscustomobject]@{ Start = $start; Seconds = ($last - $start).TotalSeconds + $IntervalMs / 1000.0; Worst = $worst }
                $start = $null
            }
        }
        if($start) { $episodes += [pscustomobject]@{ Start = $start; Seconds = ($last - $start).TotalSeconds + $IntervalMs / 1000.0; Worst = $worst } }
        foreach($e in ($episodes | Select-Object -Last 15)) {
            $worstText = if($e.Worst -ge 9999) { 'lost' } else { "$($e.Worst)ms" }
            Write-Output ("    episode at {0}  duration {1:N1}s  worst {2}" -f $e.Start.ToString('HH:mm:ss'), $e.Seconds, $worstText)
        }
    }
    Write-Output "Compare the episode times with the other PC's summary and with the game log."
}

try {
    # One probe in flight per target, collected when it completes: a lost or
    # slow reply never delays the other target, and spikes are exactly when
    # dense samples matter. Replies slower than 2 s count as lost.
    $inFlight = @{}
    while((Get-Date) -lt $deadline) {
        $tick = Get-Date
        foreach($t in $targets) {
            $probe = $inFlight[$t]
            if($probe -and $probe.Task.IsCompleted) {
                $rtt = 0; $status = 'Error'
                if($probe.Task.Status -eq 'RanToCompletion') {
                    $status = [string]$probe.Task.Result.Status
                    if($probe.Task.Result.Status -eq 'Success') { $rtt = [int]$probe.Task.Result.RoundtripTime }
                }
                $time = $probe.Sent
                $samples[$t].Add([pscustomobject]@{ Time = $time; Rtt = $rtt; Status = $status })
                "$($time.ToString('HH:mm:ss.fff')),$t,$rtt,$status" | Add-Content -LiteralPath $OutFile -Encoding UTF8
                if($status -ne 'Success' -or $rtt -ge $SpikeMs) {
                    $label = if($t -eq $Partner) { 'partner' } else { 'reference' }
                    Write-Output ("{0}  {1,-9} {2}" -f $time.ToString('HH:mm:ss.fff'), $label, $(if($status -eq 'Success') { "$rtt ms" } else { $status }))
                }
                $inFlight.Remove($t); $probe = $null
            }
            if(-not $probe) { $inFlight[$t] = @{ Task = $pingers[$t].SendPingAsync($t, 2000); Sent = $tick } }
        }
        $wait = $IntervalMs - [int]((Get-Date) - $tick).TotalMilliseconds
        if($wait -gt 0) { Start-Sleep -Milliseconds $wait }
    }
} finally {
    Write-Summary
}
