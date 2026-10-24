"""Check sensor placements against Home Assistant data.

Run after any change to where a sensor sits, or to its node. The entities,
locations and expected relations come from sensor_map.py, so the check and
the placement stay in one place.

Usage (token from $HA_TOKEN, host from sensor_map.HOST):
    python sensor_check.py --start 2026-10-06
    python sensor_check.py --start 2026-10-06 --end 2026-10-24

Output: a health table per sensor, then one row per placement relation with
a verdict. consistent = the data matches the placement; CHECK = it does not,
so look at the placement or the node before trusting the sensor there.
"""
import argparse
import bisect
import os
import statistics
import sys
from datetime import datetime, timedelta

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ha_baseline as hb  # noqa: E402
import sensor_map as M  # noqa: E402

LAG_THRESHOLD_K = 0.5
LAG_MAX = timedelta(minutes=30)
LAG_STEP = timedelta(seconds=30)
MAX_AGE = timedelta(hours=1)


def mean_sd(xs):
    m = sum(xs) / len(xs)
    sd = (sum((x - m) ** 2 for x in xs) / len(xs)) ** 0.5
    return m, sd


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--start", required=True, help="first local day, YYYY-MM-DD")
    ap.add_argument("--end", help="last local day, inclusive; default now")
    ap.add_argument("--host", default=M.HOST)
    args = ap.parse_args()

    token = os.environ.get("HA_TOKEN", "")
    if not token:
        ap.error("no token: set HA_TOKEN")

    now = datetime.now(hb.LOCAL)
    t0 = datetime.strptime(args.start, "%Y-%m-%d").replace(tzinfo=hb.LOCAL)
    t_end = (datetime.strptime(args.end, "%Y-%m-%d").replace(tzinfo=hb.LOCAL) + timedelta(days=1)
             if args.end else now)

    ids = [s["entity"] for s in M.SENSORS.values()] + list(M.BINARY.values())
    raw = {}
    t = t0 - timedelta(hours=1)
    while t < t_end:
        t2 = min(t + timedelta(days=3), t_end)
        for eid, pts in hb.fetch(args.host, token, t, t2, ids).items():
            raw.setdefault(eid, []).extend(pts)
        t = t2

    S = {k: hb.Series(sorted(set(raw.get(v["entity"], []))), t_end) for k, v in M.SENSORS.items()}
    B = {k: hb.Series(sorted(set(raw.get(v, []))), t_end) for k, v in M.BINARY.items()}
    TIMES = {k: [p[0] for p in S[k].pts] for k in S}

    def value(k, t):
        v = hb.num(S[k].at(t))
        if v is None or v > 1e6:
            return None
        cal = M.SENSORS[k].get("offset_before")
        if cal and t < cal[0]:
            v += cal[1]
        return v

    def fresh(k, t):
        i = bisect.bisect_right(TIMES[k], t) - 1
        return i >= 0 and t - TIMES[k][i] <= MAX_AGE

    # Health: unavailable/unknown count, longest silence, last value
    print(f"# Sensor check, {t0:%Y-%m-%d} to {t_end:%Y-%m-%d %H:%M}\n")
    print("## Health\n")
    print("| key | entity | location | source | changes | unavailable | longest gap h | last value |")
    print("|---|---|---|---|---|---|---|---|")
    for k, cfg in M.SENSORS.items():
        pts = S[k].pts
        unav = sum(1 for _, s in pts if hb.num(s) is None)
        gaps = [(b[0] - a[0]).total_seconds() / 3600 for a, b in zip(pts, pts[1:])]
        longest = max(gaps) if gaps else float("nan")
        last = pts[-1][1] if pts else "-"
        print(f"| {k} | {cfg['entity']} | {cfg['location']} | {cfg['source']} | {len(pts)} | "
              f"{unav} | {longest:.1f} | {last} |")

    # Compressor transitions, for the response-time test
    events = []
    prev = None
    for a, b, s in B["comp"].intervals(t0):
        if prev is not None and s != prev:
            events.append(a)
        prev = s

    def lag(k, t_on):
        base_t = t_on - timedelta(minutes=2)
        base = value(k, base_t)
        if base is None or not fresh(k, base_t):
            return None
        t = t_on
        while t <= t_on + LAG_MAX:
            v = value(k, t)
            if v is not None and abs(v - base) >= LAG_THRESHOLD_K:
                return (t - t_on).total_seconds() / 60
            t += LAG_STEP
        return None

    # 5-minute grid, compressing, no DHW or booster
    grid = []
    t = t0
    while t < t_end:
        if B["comp"].at(t) == "on" and B["dhw"].at(t) != "on" and B["htr"].at(t) != "on":
            grid.append(t)
        t += timedelta(minutes=5)

    print("\n## Placement relations\n")
    print("| a | b | relation | n | mean a-b K | sd K | lag a min | lag b min | verdict | note |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for a, b, rel, tol, note in M.PAIRS:
        diffs = []
        for t in grid:
            if fresh(a, t) and fresh(b, t):
                va, vb = value(a, t), value(b, t)
                if va is not None and vb is not None:
                    diffs.append(va - vb)
        lags_a, lags_b = [], []
        for te in events:
            if t0 + timedelta(minutes=5) <= te <= t_end - LAG_MAX and fresh(a, te) and fresh(b, te):
                la, lb = lag(a, te), lag(b, te)
                if la is not None and lb is not None:
                    lags_a.append(la)
                    lags_b.append(lb)
        if not diffs:
            print(f"| {a} | {b} | {rel} | 0 | - | - | - | - | no data | {note} |")
            continue
        m, sd = mean_sd(diffs)
        med_a = f"{statistics.median(lags_a):.1f}" if lags_a else "-"
        med_b = f"{statistics.median(lags_b):.1f}" if lags_b else "-"
        if rel in ("same", "same_level"):
            ok = abs(m) <= tol and sd <= 0.5
            if rel == "same" and lags_a and lags_b:
                ok = ok and abs(statistics.median(lags_a) - statistics.median(lags_b)) <= 1.0
        else:  # warmer
            ok = m >= 0.3
        verdict = "consistent" if ok else "CHECK"
        print(f"| {a} | {b} | {rel} | {len(diffs)} | {m:+.2f} | {sd:.2f} | {med_a} | {med_b} | {verdict} | {note} |")


if __name__ == "__main__":
    main()
