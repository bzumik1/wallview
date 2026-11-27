import subprocess, sys, json

PANEL = sys.argv[1]
MODE = sys.argv[2] if len(sys.argv) > 2 else "scroll"
REMOTE = ("rm -f /tmp/flip.log /tmp/touch.log; (FLIPMON_TIMES=1 /tmp/flipmon 16 > /dev/null 2> /tmp/flip.log &); sleep 0.5; "
          f"/tmp/touchplay {MODE} 4 2> /tmp/touch.log; sleep 2; cat /tmp/touch.log; echo ===; cat /tmp/flip.log")
out = subprocess.run(["ssh", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR", PANEL, REMOTE], capture_output=True, text=True).stdout
touch, flips = out.split("===")
marks = [(l.split()[0], float(l.split()[1])) for l in touch.strip().splitlines()]
flip_times = [float(l.split()[1]) for l in flips.strip().splitlines() if l.startswith("flip")]
results = []
for i, (kind, t) in enumerate(marks):
    if kind != "down":
        continue
    end = next((u for k, u in marks[i + 1:] if k == "up"), t + 0.1)
    window = [f for f in flip_times if t <= f <= end + 0.3]
    first = next((f for f in flip_times if f > t), None)
    gaps = [b - a for a, b in zip(window, window[1:])]
    results.append({
        "firstFrameMs": round((first - t) * 1000) if first else None,
        "frames": len(window),
        "windowMs": round((end + 0.3 - t) * 1000),
        "fps": round(len(window) / (end + 0.3 - t), 1),
        "worstGapMs": round(max(gaps) * 1000) if gaps else None,
        "missed": sum(max(0, round(g / 0.01667) - 1) for g in gaps),
    })
print(json.dumps(results))
summary = {k: [r[k] for r in results] for k in ("fps", "worstGapMs", "firstFrameMs", "missed")}
print(json.dumps(summary))
