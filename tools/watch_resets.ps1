<#
  Polls /api/status and records every reboot, so a before/after change
  (e.g. adding a bulk capacitor) can be compared on evidence.

  Usage:
    .\tools\watch_resets.ps1 -Label before
    .\tools\watch_resets.ps1 -Label after

  Ctrl+C to stop. Appends to captures\resets_<Label>.csv.
#>
param(
  [string]$Device   = "192.168.10.40",
  [string]$Label    = "before",
  [int]   $Interval = 30
)

$reasons = @{
  0='UNKNOWN'; 1='POWERON'; 2='EXT'; 3='SW'; 4='PANIC'; 5='INT_WDT';
  6='TASK_WDT'; 7='WDT'; 8='DEEPSLEEP'; 9='BROWNOUT'; 10='SDIO';
  11='USB'; 12='JTAG'; 13='EFUSE'; 14='PWR_GLITCH'; 15='CPU_LOCKUP'
}

$out = Join-Path $PSScriptRoot "..\captures\resets_$Label.csv"
if (-not (Test-Path $out)) {
  "time,reachable,uptime,event,reason,reasonName,version,heap,minHeap,rssi,connects,disconnects,pubOk,pubFail" |
    Out-File -FilePath $out -Encoding utf8
}

Write-Host "Logging $Device -> $out (every ${Interval}s). Ctrl+C to stop."
$prevUptime = $null

while ($true) {
  $now = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
  $s = $null
  try   { $s = Invoke-RestMethod -Uri "http://$Device/api/status" -TimeoutSec 8 }
  catch { $s = $null }

  if ($null -eq $s) {
    "$now,0,,unreachable,,,,,,,,,," | Out-File -FilePath $out -Append -Encoding utf8
    Write-Host "$now  UNREACHABLE" -ForegroundColor Yellow
  }
  else {
    $event = "poll"
    if ($null -ne $prevUptime -and $s.uptime -lt $prevUptime) { $event = "REBOOT" }
    $prevUptime = $s.uptime

    $name = $reasons[[int]$s.resetReason]
    if ($null -eq $name) { $name = "?" }

    "$now,1,$($s.uptime),$event,$($s.resetReason),$name,$($s.version),$($s.heap),$($s.minHeap),$($s.rssi),$($s.connects),$($s.disconnects),$($s.pubOk),$($s.pubFail)" |
      Out-File -FilePath $out -Append -Encoding utf8

    if ($event -eq "REBOOT") {
      Write-Host "$now  REBOOT  reason=$($s.resetReason) ($name)  v$($s.version)" -ForegroundColor Red
    }
  }

  Start-Sleep -Seconds $Interval
}
