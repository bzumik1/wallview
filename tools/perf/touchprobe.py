import statistics, sys


def pct(values, p):
    s = sorted(values)
    return s[min(len(s) - 1, int(len(s) * p))]


def describe(name, values):
    if not values:
        print(f"{name}: none")
        return
    ms = [v / 1e6 for v in values]
    print(f"{name}: n={len(ms)} p10={pct(ms, .1):.2f} median={statistics.median(ms):.2f} p90={pct(ms, .9):.2f} max={max(ms):.2f} ms")


for path in sys.argv[1:]:
    falls, rises, data = [], [], []
    for line in open(path):
        f = line.split()
        if f[0] == "F":
            falls.append(int(f[1]))
        elif f[0] == "R":
            rises.append(int(f[1]))
        elif f[0] == "D":
            data.append((int(f[1]), int(f[2]), int(f[3]), int(f[5]), int(f[6])))
    print(f"== {path}")
    touching = [d for d in data if d[2] > 0]
    print(f"falling edges {len(falls)}, rising {len(rises)}, reads {len(data)}, reads with touch {len(touching)}")
    describe("falling edge interval", [b - a for a, b in zip(falls, falls[1:]) if b - a < 200e6])
    pulses = []
    for f in falls:
        r = next((r for r in rises if r > f), None)
        if r:
            pulses.append(r - f)
    describe("INT low time", pulses)
    describe("read duration", [d[1] - d[0] for d in data])
    changes = [touching[0]] if touching else []
    for d in touching[1:]:
        if (d[3], d[4]) != (changes[-1][3], changes[-1][4]):
            changes.append(d)
    describe("new position interval", [b[0] - a[0] for a, b in zip(changes, changes[1:]) if b[0] - a[0] < 200e6])
    xs = [d[3] for d in touching]
    ys = [d[4] for d in touching]
    if xs:
        print(f"position spread: x {min(xs)}-{max(xs)} (stdev {statistics.pstdev(xs):.1f}), y {min(ys)}-{max(ys)} (stdev {statistics.pstdev(ys):.1f})")
    print(f"reads without touch {len(data) - len(touching)}")
