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
    # PowerMeter nodes (sibling project), /api/values. Each channel is
    # flattened to "<label>.i" amps and "<label>.p" watts.
    #
    #   .231 "PowerMeterIndoorUnit"  - indoor unit, pump and controls
    #   .238 "PowerMeterOutdoorUnit" - the compressor, on L2-L3
    #
    # Amps are the honest quantity. The watts are `V x I` with power factor
    # ASSUMED to be 1, and the compressor's measured PF runs 0.60 at minimum
    # modulation to 0.91 at full load - so those watts overstate real power by
    # up to 67 %. Use them for detecting state, never for energy.
    [string[]]$PowerMeter = @("192.168.10.231", "192.168.10.238"),
    # AMS house meter. Gives TRUE active power for the whole installation, so
    # it is the sanity check on everything above: no sub-meter may exceed it.
    # That test caught a stuck outdoor CT reading 874 W against a 778 W house.
    [string]$AmsMeter = "192.168.10.190",
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

# Keep Windows awake for the duration of the capture.
#
# On 2026-09-16 the PC entered S3 sleep at 22:24 with a 24-hour capture
# running. The script froze with it, resumed at 06:00 and immediately hit its
# end time, so the whole night - including the DHW cycle that was the point of
# the run - is a 7.6-hour hole in the log with no error anywhere. Idle sleep
# is the one failure mode that leaves the process alive and the file looking
# healthy, so it has to be blocked rather than detected.
#
# ES_CONTINUOUS | ES_SYSTEM_REQUIRED holds off IDLE sleep only; a lid close or
# an explicit Sleep from the Start menu still wins. The flag is per-thread and
# dies with the process, so nothing has to be cleaned up if this is killed.
#
# The flag is written as the decimal 2147483649, not 0x80000001: PowerShell 5.1
# parses a hex literal with the top bit set as a NEGATIVE Int32, and the cast to
# uint32 then fails with a non-terminating error - the script carries on and the
# machine sleeps anyway. A returned previous-state of 0x80000000 or 0x80000001
# confirms the call landed.
Add-Type -Name Power -Namespace Win32 -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true)]
public static extern uint SetThreadExecutionState(uint esFlags);
'@
$prev = [Win32.Power]::SetThreadExecutionState([uint32]2147483649)
if ($prev -eq 0) { Write-Warning "SetThreadExecutionState failed - the PC may sleep mid-capture" }

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

    foreach ($node in $PowerMeter) {
        if (-not $node) { continue }
        try {
            $m = Invoke-RestMethod -Uri "http://$node/api/values" -TimeoutSec 5
            $flat = [ordered]@{}
            foreach ($c in $m.ch) {
                $flat["$($c.label).i"] = $c.i
                $flat["$($c.label).p"] = $c.p
            }
            $fields += ($flat | ConvertTo-Json -Compress)
        } catch {
            $fields += "UNREACHABLE"
        }
    }

    if ($AmsMeter) {
        try {
            $h = Invoke-RestMethod -Uri "http://$AmsMeter/data.json" -TimeoutSec 5
            # Only the fields that matter: true active power, and per-phase
            # volts and amps for the "no sub-meter may exceed this" check.
            $flat = [ordered]@{ w = $h.w }
            foreach ($ph in 'l1','l2','l3') {
                $flat["$ph.u"] = $h.$ph.u
                $flat["$ph.i"] = $h.$ph.i
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
