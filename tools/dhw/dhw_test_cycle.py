import os, sys, bisect
from datetime import datetime, timedelta
sys.path.insert(0, r"D:\SteinsRootMappe\Claude\AlthermaInterface\tools")
import ha_baseline as hb
import sensor_map as M

token = os.environ["HA_TOKEN"]
now = datetime.now(hb.LOCAL)
T0 = datetime(2026, 10, 24, 10, 0, tzinfo=hb.LOCAL)
T1 = datetime(2026, 10, 24, 11, 2, tzinfo=hb.LOCAL)
keys = ["inlet", "dhw", "outlet", "returmaskin", "ford_in", "lager_ut"]
ids = [M.SENSORS[k]["entity"] for k in keys] + [M.BINARY[k] for k in ("comp", "dhw", "htr")]
raw = hb.fetch(M.HOST, token, T0 - timedelta(days=2), T1, list(set(ids)))
S = {k: hb.Series(sorted(set(raw.get(M.SENSORS[k]["entity"], []))), T1) for k in keys}
B = {k: hb.Series(sorted(set(raw.get(M.BINARY[k], []))), T1) for k in ("comp", "dhw", "htr")}
TIMES = {k: [p[0] for p in S[k].pts] for k in S}

def fresh(k, t):
    i = bisect.bisect_right(TIMES[k], t) - 1
    return i >= 0 and t - TIMES[k][i] <= timedelta(hours=1)

def num(k, t):
    v = hb.num(S[k].at(t))
    return None if v is None or v > 1e6 else v

rows = []
t = T0
while t < T1:
    if B["dhw"].at(t) == "on" and B["htr"].at(t) != "on" and all(fresh(k, t) for k in keys):
        r = {k: num(k, t) for k in keys}
        if all(r[k] is not None for k in keys):
            rows.append(r)
    t += timedelta(minutes=1)

n = len(rows)
print(f"test cycle, DHW on, booster off, fresh: n={n} ({n} min)")
if n < 5:
    raise SystemExit("too few samples")
print(f"tank: start {rows[0]['dhw']:.1f}, end {rows[-1]['dhw']:.1f} C; outlet mean {sum(r['outlet'] for r in rows)/n:.1f}; inlet mean {sum(r['inlet'] for r in rows)/n:.1f}")

def r1(xs, ys):
    mx = sum(xs)/len(xs); my = sum(ys)/len(ys)
    sxy = sum((p-mx)*(q-my) for p,q in zip(xs,ys)); sxx = sum((p-mx)**2 for p in xs); syy = sum((q-my)**2 for q in ys)
    return sxy/sxx, (sxy*sxy)/(sxx*syy) if sxx and syy else float("nan")

x = [r["outlet"] - r["dhw"] for r in rows]; y = [r["inlet"] - r["dhw"] for r in rows]
b, r2 = r1(x, y)
mx = sum(x)/n; my = sum(y)/n
print(f"inlet-tank vs outlet-tank: slope={b:+.3f}, R2={r2:.2f}; mean outlet-tank {mx:+.1f}, mean inlet-tank {my:+.1f}, share {my/mx:.0%}")
b, r2 = r1([r["dhw"] for r in rows], [r["inlet"] for r in rows])
print(f"inlet vs tank alone: slope={b:+.3f}, R2={r2:.2f}")
b, r2 = r1([r["outlet"] for r in rows], [r["inlet"] for r in rows])
print(f"inlet vs outlet alone: slope={b:+.3f}, R2={r2:.2f}")
print("baseline (20 Sep to 24 Oct, normal setpoint): slope +0.60, share ~62 %, inlet vs tank R2 0.57")

