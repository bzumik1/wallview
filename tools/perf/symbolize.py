import bisect, re, subprocess, sys
lib, report = sys.argv[1], sys.argv[2]
syms = []
for line in subprocess.run(["nm", "-C", "-n", "--defined-only", lib], capture_output=True, text=True).stdout.splitlines():
    parts = line.split(" ", 2)
    if len(parts) == 3 and parts[1].lower() in ("t", "w"):
        syms.append((int(parts[0], 16), parts[2]))
addrs = [a for a, _ in syms]
totals = {}
for line in open(report):
    m = re.match(r"\s*([\d.]+)%\s+libWPEWebKit\S*\s+\[\.\]\s+0x([0-9a-f]+)", line)
    if not m:
        continue
    pct, addr = float(m.group(1)), int(m.group(2), 16)
    i = bisect.bisect_right(addrs, addr) - 1
    name = syms[i][1] if i >= 0 else "?"
    name = re.sub(r"\(.*", "", name)[:110]
    totals[name] = totals.get(name, 0) + pct
for name, pct in sorted(totals.items(), key=lambda x: -x[1])[:45]:
    print(f"{pct:6.2f}%  {name}")
