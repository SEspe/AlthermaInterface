import os, sys, bisect
from datetime import datetime, timedelta
sys.path.insert(0, r"D:\SteinsRootMappe\Claude\AlthermaInterface\tools")
import ha_baseline as hb
import sensor_map as M

token = os.environ["HA_TOKEN"]
now = datetime.now(hb.LOCAL)
T0 = datetime(2026, 9, 20, tzinfo=hb.LOCAL)
keys = ["inlet", "dhw", "outlet", "returmaskin", "ford_in", "lager_ut"]
ids = [M.SENSORS[k]["entity"] for k in keys] + [M.BINARY[k] for k in ("comp", "dhw", "htr")]
raw = {}
t = T0 - timedelta(hours=1)
while t < now:
    t2 = min(t + timedelta(days=3), now)
    for eid, pts in hb.fetch(M.HOST, token, t, t2, ids).items():
        raw.setdefault(eid, []).extend(pts)
    t = t2
S = {k: hb.Series(sorted(set(raw.get(M.SENSORS[k]["entity"], []))), now) for k in keys}
B = {k: hb.Series(sorted(set(raw.get(M.BINARY[k], []))), now) for k in ("comp", "dhw", "htr")}
TIMES = {k: [p[0] for p in S[k].pts] for k in S}

def fresh(k, t):
    i = bisect.bisect_right(TIMES[k], t) - 1
    return i >= 0 and t - TIMES[k][i] <= timedelta(hours=1)

def num(k, t):
    v = hb.num(S[k].at(t))
    return None if v is None or v > 1e6 else v

rows = []
t = T0
while t < now:
    if B["dhw"].at(t) == "on" and B["htr"].at(t) != "on" and all(fresh(k, t) for k in keys):
        r = {k: num(k, t) for k in keys}
        if all(r[k] is not None for k in keys):
            rows.append(r)
    t += timedelta(minutes=5)

n = len(rows)
print(f"DHW-on samples, fresh, no booster: n={n} ({n*5/60:.1f} h)")

def ols2(rows, x1, x2, y):
    m1 = sum(r[x1] for r in rows) / n; m2 = sum(r[x2] for r in rows) / n; my = sum(r[y] for r in rows) / n
    s11 = s22 = s12 = s1y = s2y = syy = 0.0
    for r in rows:
        a = r[x1] - m1; b = r[x2] - m2; c = r[y] - my
        s11 += a * a; s22 += b * b; s12 += a * b; s1y += a * c; s2y += b * c; syy += c * c
    det = s11 * s22 - s12 * s12
    b1 = (s1y * s22 - s2y * s12) / det
    b2 = (s2y * s11 - s1y * s12) / det
    a0 = my - b1 * m1 - b2 * m2
    r2 = (b1 * s1y + b2 * s2y) / syy
    return a0, b1, b2, r2

a0, b1, b2, r2 = ols2(rows, "dhw", "outlet", "inlet")
print(f"inlet = {a0:+.2f} + {b1:+.3f}*tank + {b2:+.3f}*outlet, R2={r2:.2f}")
pass
pass

# leak test as a plain difference: does inlet-tank grow with outlet-tank?
x = [r["outlet"] - r["dhw"] for r in rows]
y = [r["inlet"] - r["dhw"] for r in rows]
mx = sum(x) / n; my = sum(y) / n
b = sum((p - mx) * (q - my) for p, q in zip(x, y)) / sum((p - mx) ** 2 for p in x)
print(f"inlet-tank vs outlet-tank: slope={b:+.3f}, mean outlet-tank={mx:+.1f}, mean inlet-tank={my:+.1f}")

# does the inlet rise in step with the outlet, or lag it? cross-correlation of 5-min changes
dx = [rows[i+1]["outlet"] - rows[i]["outlet"] for i in range(n-1)]
dy = [rows[i+1]["inlet"] - rows[i]["inlet"] for i in range(n-1)]
def pear(a, b):
    ma = sum(a)/len(a); mb = sum(b)/len(b)
    return sum((p-ma)*(q-mb) for p,q in zip(a,b)) / (sum((p-ma)**2 for p in a)*sum((q-mb)**2 for q in b))**0.5
for lag in [-1, 0, 1]:
    if lag >= 0:
        a, c = dx[:len(dx)-lag] if lag else dx, dy[lag:]
    else:
        a, c = dx[-lag:], dy[:len(dy)+lag]
    print(f"  5-min changes, outlet vs inlet, lag {lag*5:+d} min: r={pear(a, c):+.2f}")

def r1(xs, ys):
    mx = sum(xs)/len(xs); my = sum(ys)/len(ys)
    b = sum((p-mx)*(q-my) for p,q in zip(xs,ys)) / sum((p-mx)**2 for p in xs)
    r2 = (sum((p-mx)*(q-my) for p,q in zip(xs,ys))**2) / (sum((p-mx)**2 for p in xs)*sum((q-my)**2 for q in ys))
    return b, r2
b, r2 = r1([r["dhw"] for r in rows], [r["inlet"] for r in rows])
print(f"inlet vs tank alone:   slope={b:+.3f}, R2={r2:.2f}")
b, r2 = r1([r["outlet"] for r in rows], [r["inlet"] for r in rows])
print(f"inlet vs outlet alone: slope={b:+.3f}, R2={r2:.2f}")
b, r2 = r1([r["dhw"] for r in rows], [r["outlet"] for r in rows])
print(f"outlet vs tank (collinearity): slope={b:+.3f}, R2={r2:.2f}")
