# Records a HUMAN-OBSERVED compressor state against every sensor at that moment.
#
# The Altherma indoor unit's display carries a dedicated compressor icon -
# operation manual item 15, "this icon indicates that the compressor in the
# outdoor unit of the installation is active". That is the only direct
# statement of compressor state available on this installation: X10A does not
# report it (it is inferred from the water delta), and the outdoor CT has been
# caught reading a load-independent constant.
#
# So this script exists to build a labelled dataset: the observer calls the
# state, the script captures what every instrument said at that instant, and
# the two can afterwards be compared honestly.
#
#   .\mark_compressor.ps1 ON      # icon lit
#   .\mark_compressor.ps1 OFF     # icon dark
#   .\mark_compressor.ps1 DEFROST # defrost/startup icon (manual item 14)
#
# One CSV row per call. Append-only; the header is written once.

param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('ON', 'OFF', 'DEFROST')]
    [string]$State,
    [string]$Note     = "",
    [string]$Device   = "192.168.10.40",
    [string]$Outdoor  = "192.168.10.238",
    [string]$Indoor   = "192.168.10.231",
    [string]$AmsMeter = "192.168.10.190",
    [string]$EspEasy  = "192.168.10.160",
    [string]$OutDir   = (Join-Path (Split-Path $PSScriptRoot -Parent) "captures")
)

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force $OutDir | Out-Null }
$csv = Join-Path $OutDir ("compressor_truth_{0}.csv" -f (Get-Date -Format "yyyy-MM-dd"))

function Try-Get($uri) {
    try { return Invoke-RestMethod -Uri $uri -TimeoutSec 5 } catch { return $null }
}

$t   = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
$a   = Try-Get "http://$Device/api/values"
$out = Try-Get "http://$Outdoor/api/values"
$ind = Try-Get "http://$Indoor/api/values"
$ams = Try-Get "http://$AmsMeter/data.json"
$esp = Try-Get "http://$EspEasy/json"

$v = @{}
if ($a) { foreach ($x in $a.values) { $v[$x.label] = $x.value } }

$ute = $null
if ($esp) {
    foreach ($s in $esp.Sensors) {
        foreach ($x in $s.TaskValues) { if ($x.Name -eq 'ute03') { $ute = $x.Value } }
    }
}

$inlet  = [double]$v['Inlet water temp.(C)']
$outlet = [double]$v['Outlet Water Temp.(C)']

$row = [ordered]@{
    time        = $t
    observed    = $State
    derived     = $v['Compressor']
    dT          = if ($inlet -and $outlet) { [math]::Round($outlet - $inlet, 2) } else { $null }
    inlet       = $inlet
    outlet      = $outlet
    refrig      = $v['Refrig. Temp. liquid side(C)']
    pump        = $v['Circulation pump']
    dhw_prio    = $v['Priority to domestic water']
    heater      = $v['Electric heater contactor']
    outdoor_i   = if ($out) { [math]::Round($out.ch[0].i, 3) } else { $null }
    outdoor_va  = if ($out) { [math]::Round($out.ch[0].p, 1) } else { $null }
    indoor_i    = if ($ind) { [math]::Round(($ind.ch | Measure-Object -Property i -Sum).Sum, 3) } else { $null }
    house_w     = if ($ams) { $ams.w } else { $null }
    l1_i        = if ($ams) { $ams.l1.i } else { $null }
    l2_i        = if ($ams) { $ams.l2.i } else { $null }
    l3_i        = if ($ams) { $ams.l3.i } else { $null }
    ute03       = $ute
    note        = $Note
}

if (-not (Test-Path $csv)) {
    ($row.Keys -join ',') | Out-File -FilePath $csv -Encoding utf8
}
(($row.Values | ForEach-Object { if ($_ -is [string] -and $_ -match ',') { '"' + $_ + '"' } else { $_ } }) -join ',') |
    Out-File -FilePath $csv -Append -Encoding utf8

"{0}  observed={1}  derived={2}  dT={3}  outdoor={4} A  house={5} W" -f `
    $t, $State, $row.derived, $row.dT, $row.outdoor_i, $row.house_w
