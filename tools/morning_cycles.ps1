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
    # ESPEasy nodes carrying DS18B20 sensors on the same system. Pass an empty
    # array to skip them.
    #
    #   .160 "Altherma"  - heat pump flow/return, buffer tank, room, and the
    #                      OUTDOOR temperature (ute03). X10A reports nothing
    #                      from the outdoor unit, so without ute03 a changed
    #                      start rate cannot be told apart from changed
    #                      weather - the whole point of a before/after test.
    #   .162 "Altherma2" - the tank and the two distribution lines:
    #                        temp2 into the buffer tank, temp3 out of it
    #                        temp4 / temp5  flow / return, first and second
    #                                       floor heating (four circuits)
    #                        temp6 / temp1  flow / return, basement floor
    #                      temp4-temp5 and temp6-temp1 are the heat actually
    #                      reaching the house, which is what decides how long
    #                      the compressor can run before the tank is charged.
    #                      Its SHT temperature/humidity task is INTERMITTENT:
    #                      it returned 2^64, the ESPEasy invalid marker, on
    #                      two polls and valid values minutes later. Treat any
    #                      reading above ~1e17 as missing rather than as data.
    #
    # Each node's values are flattened and written as their own field, keyed by
    # task name - .162 names every value "temperature", so keying on the value
    # name alone collapses all six sensors into one.
    #   .161 "Climate1"  - the LIVING ROOM: a DS18B20 (Temp1) plus a BME280
    #                      and a CO2 sensor. Note the BME280 sits beside the
    #                      ESP32 and self-heats, reading roughly 1 K above the
    #                      DS18B20 - prefer Temp1. This is the only view of
    #                      whether the house actually responds to the heating;
    #                      romtemp94 on .160 is the TECHNICAL room, which is
    #                      warm for reasons that have nothing to do with the
    #                      house.
    [string[]]$EspEasy = @("192.168.10.160", "192.168.10.161", "192.168.10.162"),
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
    } catch {
        # A single failed poll must not end the run - the device reboots on OTA
        # and after any heat pump power cycle.
        $json = "UNREACHABLE"
    }

    $fields = @($t, $json)
    foreach ($node in $EspEasy) {
        if (-not $node) { continue }
        try {
            $e = Invoke-RestMethod -Uri "http://$node/json" -TimeoutSec 5
            $flat = [ordered]@{}
            foreach ($s in $e.Sensors) {
                $vals = @($s.TaskValues)
                foreach ($v in $vals) {
                    # One value per task: use the task name. Several: qualify
                    # it, so same-named values cannot overwrite each other.
                    $key = if ($vals.Count -eq 1) { $s.TaskName } else { "$($s.TaskName).$($v.Name)" }
                    $flat[$key] = $v.Value
                }
            }
            $fields += ($flat | ConvertTo-Json -Compress)
        } catch {
            $fields += "UNREACHABLE"
        }
    }

    ($fields -join '|') | Out-File -FilePath $log -Append -Encoding utf8
    Start-Sleep -Seconds $IntervalSec
}

"# finished $(Get-Date -Format 's')" | Out-File -FilePath $log -Append -Encoding utf8
