import os, sys, bisect
from datetime import datetime, timedelta
sys.path.insert(0, r"D:\SteinsRootMappe\Claude\AlthermaInterface\tools")
import ha_baseline as hb
import sensor_map as M

token = os.environ["HA_TOKEN"]
now = datetime.now(hb.LOCAL)
T0 = datetime(2026, 10, 10, tzinfo=hb.LOCAL)
keys = ["inlet", "dhw", "outlet", "lager_ut", "turlager"]
ids = [M.SENSORS[k]["entity"] for k in keys] + [M.BINARY[k] for k in ("comp", "dhw", "htr")]
raw = hb.fetch(M.HOST, token, T0 - timedelta(days=2), now, list(set(ids)))
S = {k: hb.Series(sorted(set(raw.get(M.SENSORS[k]["entity"], []))), now) for k in keys}
B = {k: hb.Series(sorted(set(raw.get(M.BINARY[k], []))), now) for k in ("comp", "dhw", "htr")}
TIMES = {k: [p[0] for p in S[k].pts] for k in S}

def fresh(k, t):
    i = bisect.bisect_right(TIMES[k], t) - 1
    return i >= 0 and t - TIMES[k][i] <= timedelta(hours=1)

def num(k, t):
    v = hb.num(S[k].at(t))
    return None if v is None or v > 1e6 else v

cycles = [(a, b) for a, b, s in B["dhw"].intervals(T0) if s == "on" and (b - a) >= timedelta(minutes=5)]
print(f"DHW cycles since {T0:%d %b}: {len(cycles)}")
print("start        min  tank s->e     booster min  n   inlet-tank  outlet-tank  share   slope  buf10  buf11")
for a, b in cycles:
    rows = []
    t = a
    while t < b:
        if B["htr"].at(t) != "on" and all(fresh(k, t) for k in keys):
            r = {k: num(k, t) for k in keys}
            if all(r[k] is not None for k in keys):
                rows.append(r)
        t += timedelta(minutes=1)
    boost = sum((min(y, b) - max(x, a)).total_seconds() for x, y, s in B["htr"].intervals(a) if s == "on" and y > a and x < b) / 60
    tank_s = num("dhw", a); tank_e = num("dhw", b)
    mins = (b - a).total_seconds() / 60
    if len(rows) < 5:
        print(f"{a:%d %b %H:%M}  {mins:4.0f}  {tank_s}->{tank_e}   {boost:5.0f}   {len(rows):3d}  too few samples")
        continue
    n = len(rows)
    it = sum(r["inlet"] - r["dhw"] for r in rows) / n
    ot = sum(r["outlet"] - r["dhw"] for r in rows) / n
    share = it / ot if ot > 0.5 else float("nan")
    x = [r["outlet"] - r["dhw"] for r in rows]; y = [r["inlet"] - r["dhw"] for r in rows]
    mx = sum(x)/n; my = sum(y)/n
    sxx = sum((p-mx)**2 for p in x)
    slope = sum((p-mx)*(q-my) for p,q in zip(x,y))/sxx if sxx > 0 else float("nan")
    b10 = sum(r["turlager"] for r in rows)/n; b11 = sum(r["lager_ut"] for r in rows)/n
    print(f"{a:%d %b %H:%M}  {mins:4.0f}  {tank_s:5.1f}->{tank_e:5.1f}  {boost:5.0f}   {n:3d}   {it:+7.1f}    {ot:+7.1f}     {share:5.0%}  {slope:+5.2f}  {b10:5.1f}  {b11:5.1f}")
