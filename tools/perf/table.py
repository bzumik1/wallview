import json, sys
data = json.load(sys.stdin)
for r in data:
    i = r["idle"]; s = r["scroll"] or {}
    print(f"{str(r['view']):10} el={r['elements']:5} idle fps={i['fps']:5} p50={i['p50']:3} p95={i['p95']:4} worst={i['worst']:5} slow={i['over20']:3}/{i['n']:3} | scroll fps={s.get('fps','-')} p95={s.get('p95','-')} | anim: {r['animations'][:90]}")
