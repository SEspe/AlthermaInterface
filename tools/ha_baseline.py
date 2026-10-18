"""Heat pump performance metrics from Home Assistant history.

Recomputes the same figures for any date range, so a later check can be
compared against a saved baseline using identical definitions.

Usage (token kept outside the repo):
    python ha_baseline.py --start 2026-09-20 --end 2026-10-18 --token-file <path>
    python ha_baseline.py --start 2026-09-20 --end 2026-10-18     # uses $HA_TOKEN

Definitions (all times Europe/Oslo):
  starts      OFF -> ON transitions of binary_sensor.espaltherma_compressor
  duty        share of time the compressor is on
  run / off   median length of compressor on / off periods
  booster     minutes the electric heater contactor is on, excluding
              disinfection. A disinfection episode is one longer than 50 min
              that ends with the DHW tank at or above 55 C.
  outlet p90  90th percentile of outlet water while the compressor is on and
              neither DHW nor the booster is active, by 2 C outdoor band.
  ams_kwh     house energy from the AMS cumulative total (sensor.ams_cf58_tpi)

Home Assistant's history endpoint returns one day by default, so end_time is
always passed explicitly.
"""

import argparse
import bisect
import json
import os
import statistics
import urllib.parse
import urllib.request
from collections import defaultdict
from datetime import datetime, timedelta

try:
    from zoneinfo import ZoneInfo
    LOCAL = ZoneInfo("Europe/Oslo")
except Exception:  # tzdata missing on this Python
    from datetime import timezone
    LOCAL = timezone(timedelta(hours=2))  # CEST; wrong in winter

ENT = {
    "comp": "binary_sensor.espaltherma_compressor",
    "out": "sensor.espaltherma_outlet_water_tempc",
    "inl": "sensor.espaltherma_inlet_water_tempc",
    "tank": "sensor.espaltherma_dhw_tank_tempc",
    "dhw": "binary_sensor.espaltherma_priority_to_domestic_water",
    "htr": "binary_sensor.espaltherma_electric_heater_contactor",
    "ute": "sensor.tempute",
    "room": "sensor.tempstue",
    "ams": "sensor.ams_cf58_tpi",
}
KW_BOOSTER = 2.83
DISINFECT_MIN_MINUTES = 50
DISINFECT_TANK_C = 55.0


def fetch(host, token, start, end, entities):
    url = (f"{host}/api/history/period/{start:%Y-%m-%dT%H:%M:%SZ}"
           f"?end_time={end:%Y-%m-%dT%H:%M:%SZ}"
           "&minimal_response&no_attributes&significant_changes_only=0"
           "&filter_entity_id=" + urllib.parse.quote(",".join(entities)))
    req = urllib.request.Request(url, headers={"Authorization": "Bearer " + token})
    data = json.load(urllib.request.urlopen(req, timeout=300))
    out = {}
    for series in data:
        if not series:
            continue
        pts = []
        for p in series:
            if "last_changed" not in p:
                continue
            t = datetime.fromisoformat(p["last_changed"]).astimezone(LOCAL)
            pts.append((t, p["state"]))
        out[series[0]["entity_id"]] = sorted(pts, key=lambda x: x[0])
    return out


class Series:
    def __init__(self, pts, end):
        self.pts = pts
        self.end = end
        self.times = [t for t, _ in pts]

    def at(self, t):
        i = bisect.bisect_right(self.times, t) - 1
        return self.pts[i][1] if i >= 0 else None

    def intervals(self, start):
        # Clip to [start, end). The endpoint can return a change after `end`,
        # which would otherwise give a negative duration.
        for i, (t, s) in enumerate(self.pts):
            if t >= self.end:
                break
            t2 = self.pts[i + 1][0] if i + 1 < len(self.pts) else self.end
            t2 = min(t2, self.end)
            if t2 <= start:
                continue
            yield max(t, start), t2, s


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(int(p * len(xs)), len(xs) - 1)] if xs else float("nan")


def compute(raw, start, end):
    S = {k: Series(raw.get(v, []), end) for k, v in ENT.items()}
    last_day = end - timedelta(days=1)
    result = {"window": [f"{start:%Y-%m-%d}", f"{last_day:%Y-%m-%d}"]}

    # Compressor: transitions, run and off lengths, duty
    comp_iv = list(S["comp"].intervals(start))
    on_s = sum((t2 - t).total_seconds() for t, t2, s in comp_iv if s == "on")
    all_s = sum((t2 - t).total_seconds() for t, t2, s in comp_iv)
    runs, offs, starts, prev = [], [], 0, None
    for t, t2, s in comp_iv:
        dur = (t2 - t).total_seconds() / 60
        (runs if s == "on" else offs).append(dur)
        if prev == "off" and s == "on":
            starts += 1
        prev = s
    days = (end - start).total_seconds() / 86400
    result["starts_per_day"] = round(starts / days, 1)
    result["starts_per_hour"] = round(starts / (days * 24), 2)
    result["duty_pct"] = round(100 * on_s / all_s, 1) if all_s else None
    result["median_run_min"] = round(statistics.median(runs), 1) if runs else None
    result["median_off_min"] = round(statistics.median(offs), 1) if offs else None

    # Booster, excluding disinfection
    booster_min = 0.0
    disinfect = 0
    htr_iv = list(S["htr"].intervals(start))
    for t, t2, s in htr_iv:
        if s != "on":
            continue
        mins = (t2 - t).total_seconds() / 60
        tank_end = num(S["tank"].at(t2))
        if mins > DISINFECT_MIN_MINUTES and tank_end is not None and tank_end >= DISINFECT_TANK_C:
            disinfect += 1
        else:
            booster_min += mins
    result["booster_min_per_day"] = round(booster_min / days, 1)
    result["booster_kwh_per_day"] = round(booster_min / days / 60 * KW_BOOSTER, 2)
    result["disinfection_cycles"] = disinfect

    # Outdoor and room means (time-weighted)
    def tw_mean(key):
        acc = wsum = 0.0
        for t, t2, s in S[key].intervals(start):
            v = num(s)
            if v is None or v > 1e6:
                continue
            secs = (t2 - t).total_seconds()
            acc += v * secs
            wsum += secs
        return round(acc / wsum, 1) if wsum else None
    result["outdoor_mean_c"] = tw_mean("ute")
    result["room_mean_c"] = tw_mean("room")

    # AMS house energy from the cumulative total
    ams = [(t, num(s)) for t, s in S["ams"].pts if num(s) is not None and t >= start - timedelta(days=1)]
    before = [v for t, v in ams if t < start]
    inside = [v for t, v in ams if start <= t <= end]
    if before and inside:
        result["ams_kwh_per_day"] = round((inside[-1] - before[-1]) / days, 1)
    else:
        result["ams_kwh_per_day"] = None

    # Outlet p90 by outdoor band, compressing and not DHW / booster
    bands = defaultdict(list)
    for t, t2, s in S["out"].intervals(start):
        v = num(s)
        if v is None:
            continue
        if S["comp"].at(t) != "on" or S["dhw"].at(t) == "on" or S["htr"].at(t) == "on":
            continue
        u = num(S["ute"].at(t))
        if u is None:
            continue
        bands[int(u // 2) * 2].append(v)
    result["outlet_p90_by_band"] = {
        f"{b}..{b + 2}": round(pct(xs, 0.9), 1) for b, xs in sorted(bands.items()) if len(xs) >= 30
    }
    return result


def markdown(res):
    lines = [f"Window {res['window'][0]} to {res['window'][1]} (full days)", ""]
    for k in ["starts_per_day", "starts_per_hour", "duty_pct", "median_run_min", "median_off_min",
              "booster_min_per_day", "booster_kwh_per_day", "disinfection_cycles",
              "outdoor_mean_c", "room_mean_c", "ams_kwh_per_day"]:
        lines.append(f"- {k}: {res[k]}")
    lines += ["", "Outlet p90 by outdoor band (compressing, no DHW, no booster):"]
    for band, v in res["outlet_p90_by_band"].items():
        lines.append(f"- {band} C: {v}")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--start", required=True, help="first local day, YYYY-MM-DD")
    ap.add_argument("--end", required=True, help="last local day, inclusive, YYYY-MM-DD")
    ap.add_argument("--host", default="http://192.168.10.151:8123")
    ap.add_argument("--token-file", help="file containing the HA long-lived token")
    ap.add_argument("--json", help="also write the metrics to this JSON file")
    args = ap.parse_args()

    token = open(args.token_file, encoding="utf-8").read().strip() if args.token_file else os.environ.get("HA_TOKEN", "")
    if not token:
        ap.error("no token: pass --token-file or set HA_TOKEN")

    start = datetime.strptime(args.start, "%Y-%m-%d").replace(tzinfo=LOCAL)
    end = datetime.strptime(args.end, "%Y-%m-%d").replace(tzinfo=LOCAL) + timedelta(days=1)
    raw = fetch(args.host, token, start - timedelta(days=1), end, list(ENT.values()))
    res = compute(raw, start, end)
    print(markdown(res))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(res, fh, indent=2)


if __name__ == "__main__":
    main()
