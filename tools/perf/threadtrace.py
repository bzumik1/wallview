import bisect, collections, re, subprocess, sys
lib, cap = sys.argv[1], sys.argv[2]
mode = sys.argv[3] if len(sys.argv) > 3 else "misses"
syms = []
for line in subprocess.run(["nm", "-C", "-n", "--defined-only", lib], capture_output=True, text=True).stdout.splitlines():
    parts = line.split(" ", 2)
    if len(parts) == 3 and parts[1].lower() in ("t", "w"):
        syms.append((int(parts[0], 16), parts[2]))
addrs = [a for a, _ in syms]
def sym(off):
    i = bisect.bisect_right(addrs, off) - 1
    return re.sub(r"\(.*", "", syms[i][1])[:90] if i >= 0 else "?"
lines = open(cap).read().split("\n")
names, bases, touch, flips, ev = {}, {}, [], [], []
section = "head"
pid = ui = None
for l in lines:
    if l.startswith("==="):
        section = l[3:]; continue
    if section == "head":
        m = re.match(r"([0-9a-f]+)-[0-9a-f]+ r-xp .*libWPEWebKit", l)
        if m: base = int(m.group(1), 16)
        m = re.match(r"TID (\d+) (.*)", l)
        if m: names[int(m.group(1))] = m.group(2)
        m = re.match(r"PID (\d+) UI (\d+)", l)
        if m: pid, ui = int(m.group(1)), int(m.group(2))
    elif section == "TOUCH":
        p = l.split()
        if len(p) == 2: touch.append((float(p[1]), p[0]))
    elif section == "FLIP":
        p = l.split()
        if len(p) == 2 and p[0] == "flip": flips.append(float(p[1]))
    elif section == "PERF":
        m = re.match(r"\s*(\d+)\s+([\d.]+):\s+(\S+)\s*(.*)", l)
        if not m: continue
        tid, t, kind, rest = int(m.group(1)), float(m.group(2)), m.group(3), m.group(4)
        if kind == "PERF_RECORD_SWITCH":
            ev.append((t, tid, "IN" if rest.strip().startswith("IN") else "OUT", ""))
        else:
            mm = re.match(r"([0-9a-f]+) \((.*)\)", rest.strip())
            if not mm: continue
            ip, dso = int(mm.group(1), 16), mm.group(2)
            if "libWPEWebKit" in dso and names.get(tid) and tid in names and base <= ip:
                s = sym(ip - base) if ip - base < 0x7000000 else "uiproc:" + hex(ip)
            else:
                s = dso.rsplit("/", 1)[-1]
            ev.append((t, tid, "S", s))
ev.sort()
short = {"WPEWebProcess": "MAIN", "eadedCompositor": "COMP", "Core: Scrolling": "SCRL", "EventDispatcher": "EVDI", "ReceiveQueue": "RCVQ", "wallview-browse": "UI", "SyncObserverDRM": "VBL", "SkiaGPUWorker": "SKIA"}
def nm(tid): return short.get(names.get(tid, "?"), names.get(tid, "?")) + ("" if tid in (pid, ui) or names.get(tid) not in ("ReceiveQueue", "wallview-browse") else str(tid % 100))
def intervals(tid):
    out, start = [], None
    for t, i, k, s in ev:
        if i != tid: continue
        if k == "IN": start = t
        elif k == "OUT" and start is not None:
            out.append((start, t)); start = None
    return out
def dump(t0, t1):
    cur = {}
    for t, tid, k, s in ev:
        if t < t0 or t > t1: continue
        if k == "IN": cur[tid] = [t, collections.Counter()]
        elif k == "S":
            if tid in cur: cur[tid][1][s] += 1
            else: cur[tid] = [t, collections.Counter({s: 1})]
        elif k == "OUT" and tid in cur:
            st, c = cur.pop(tid)
            top = ", ".join(f"{a}x{b}" for a, b in c.most_common(3))
            print(f"   {(st - t0) * 1000:8.2f} +{(t - st) * 1000:6.2f}ms {nm(tid):7s} {top}")
if mode == "misses":
    downs = [t for t, k in touch if k == "down"]; ups = [t for t, k in touch if k == "up"]
    for d, u in zip(downs, ups):
        w = [f for f in flips if d <= f <= u + 0.3]
        for a, b in zip(w, w[1:]):
            if b - a > 0.025:
                print(f"MISS swipe@{d:.3f} gap {1000 * (b - a):.0f}ms after flip {a:.6f} (t={1000 * (a - d):.0f}ms into swipe)")
                dump(a - 0.035, b + 0.002)
                marks = [f for f in flips if a - 0.04 <= f <= b + 0.01]
                print("   flips at", [round((f - (a - 0.035)) * 1000, 1) for f in marks])
elif mode == "window":
    dump(float(sys.argv[4]), float(sys.argv[5]))
