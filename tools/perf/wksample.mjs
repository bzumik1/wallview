const [host, seconds] = process.argv.slice(2);
const ws = new WebSocket(`ws://${host}/socket/1/1/WebPage`);
let id = 0, targetId = null; const pending = new Map(); let complete = null;
function send(method, params = {}) { const msgId = ++id; const message = { id: msgId, method, params }; return new Promise((resolve) => { pending.set(msgId, resolve); if (targetId) ws.send(JSON.stringify({ id: ++id, method: "Target.sendMessageToTarget", params: { targetId, message: JSON.stringify(message) } })); else ws.send(JSON.stringify(message)); }); }
function handle(m) { if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); return; } if (m.method === "ScriptProfiler.trackingComplete") complete = m.params; }
ws.onmessage = (e) => { const d = JSON.parse(e.data); if (d.method === "Target.targetCreated" && !targetId) targetId = d.params.targetInfo.targetId; if (d.method === "Target.dispatchMessageFromTarget") return handle(JSON.parse(d.params.message)); handle(d); };
ws.onopen = async () => {
  await new Promise((r) => setTimeout(r, 500));
  await send("Page.enable");
  await send("ScriptProfiler.startTracking", { includeSamples: true });
  await new Promise((r) => setTimeout(r, Number(seconds) * 1000));
  await send("ScriptProfiler.stopTracking");
  for (let i = 0; i < 40 && !complete; i++) await new Promise((r) => setTimeout(r, 250));
  const traces = complete?.samples?.stackTraces || [];
  const self = {}, total = {};
  for (const t of traces) {
    const frames = t.stackFrames || [];
    if (!frames.length) continue;
    const name = (f) => `${f.name || "(anon)"} ${String(f.url || "").split("/").pop()}:${f.line}:${f.column}`;
    self[name(frames[0])] = (self[name(frames[0])] || 0) + 1;
    for (const f of new Set(frames.map(name))) total[f] = (total[f] || 0) + 1;
  }
  console.log("samples", traces.length);
  console.log("--- self");
  console.log(Object.entries(self).sort((a, b) => b[1] - a[1]).slice(0, 20).map(([k, v]) => `${v}  ${k.slice(0, 120)}`).join("\n"));
  console.log("--- total");
  console.log(Object.entries(total).sort((a, b) => b[1] - a[1]).slice(0, 25).map(([k, v]) => `${v}  ${k.slice(0, 120)}`).join("\n"));
  process.exit(0);
};
