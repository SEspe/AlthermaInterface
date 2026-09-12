# Logs compressor cycling from the AlthermaInterface device for a fixed window.
#
# Purpose: a like-for-like comparison against the 2026-09-12 07:07 baseline
# (11.5 starts/hour with field setting [9-04]=1), taken at the same time of day
# and, ideally, a similar outdoor temperature. Run by a scheduled task at 06:55
# so the window brackets 07:00.
#
# Output is one line per poll: timestamp then the raw /api/values JSON, the same
# shape the ad-hoc bash loggers produced, so the same analysis works on it.

param(
    [string]$Device   = "192.168.10.40",
    [int]$Minutes     = 35,
    [int]$IntervalSec = 20,
    # Defaults to captures/ beside this script's parent, so the scheduled task
    # needs no absolute path and the script stays portable.
    [string]$OutDir   = (Join-Path (Split-Path $PSScriptRoot -Parent) "captures")
)

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force $OutDir | Out-Null }

$stamp = Get-Date -Format "yyyy-MM-dd_HHmm"
$log   = Join-Path $OutDir "cycles_$stamp.log"
$end   = (Get-Date).AddMinutes($Minutes)

"# AlthermaInterface cycle log, started $(Get-Date -Format 's'), device $Device" |
    Out-File -FilePath $log -Encoding utf8

while ((Get-Date) -lt $end) {
    $t = Get-Date -Format "HH:mm:ss"
    try {
        $r = Invoke-RestMethod -Uri "http://$Device/api/values" -TimeoutSec 5
        # Compact back to JSON so the line matches what the bash loggers wrote.
        $json = $r | ConvertTo-Json -Compress -Depth 5
        "$t|$json" | Out-File -FilePath $log -Append -Encoding utf8
    } catch {
        # A single failed poll must not end the run - the device reboots on OTA
        # and after any heat pump power cycle.
        "$t|UNREACHABLE" | Out-File -FilePath $log -Append -Encoding utf8
    }
    Start-Sleep -Seconds $IntervalSec
}

"# finished $(Get-Date -Format 's')" | Out-File -FilePath $log -Append -Encoding utf8
