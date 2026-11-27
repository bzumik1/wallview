const host = process.argv[2];
const expression = process.argv[3];
const ws = new WebSocket(`ws://${host}/socket/1/1/WebPage`);
let id = 0, targetId = null;
const pending = new Map();
const records = [];
const samples = [];
function send(method, params = {}) {
  const msgId = ++id;
  const message = { id: msgId, method, params };
  return new Promise((resolve) => {
    pending.set(msgId, resolve);
    if (targetId) ws.send(JSON.stringify({ id: ++id, method: "Target.sendMessageToTarget", params: { targetId, message: JSON.stringify(message) } }));
    else ws.send(JSON.stringify(message));
  });
}
function handle(msg) {
  if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); return; }
  if (msg.method === "Timeline.eventRecorded") records.push(msg.params.record);
  if (msg.method === "ScriptProfiler.trackingComplete") samples.push(msg.params);
}
ws.onmessage = (e) => {
  const d = JSON.parse(e.data);
  if (d.method === "Target.targetCreated" && !targetId) targetId = d.params.targetInfo.targetId;
  if (d.method === "Target.dispatchMessageFromTarget") return handle(JSON.parse(d.params.message));
  handle(d);
};
const totals = {};
function walk(r, depth) {
  const dur = (r.endTime ?? r.startTime) - r.startTime;
  const childDur = (r.children || []).reduce((a, c) => a + ((c.endTime ?? c.startTime) - c.startTime), 0);
  const self = Math.max(0, dur - childDur);
  const key = r.type + (r.type === "FunctionCall" || r.type === "EvaluateScript" ? "" : "");
  totals[key] = (totals[key] || 0) + self;
  (r.children || []).forEach((c) => walk(c, depth + 1));
}
ws.onopen = async () => {
  await new Promise((r) => setTimeout(r, 500));
  await send("Page.enable");
  await send("Timeline.enable");
  await send("Timeline.setInstruments", { instruments: ["ScriptProfiler", "Timeline", "CPU"] }).catch(() => {});
  await send("Runtime.evaluate", { expression: `window.__bench = undefined; (${expression})().then(r => window.__bench = r, e => window.__bench = {error: String(e)}); 1`, returnByValue: true });
  await new Promise((r) => setTimeout(r, 4000));
  await send("Timeline.start", { maxCallStackDepth: 5 });
  let result;
  for (let i = 0; i < 60; i++) {
    await new Promise((r) => setTimeout(r, 500));
    const r = await send("Runtime.evaluate", { expression: "JSON.stringify(window.__bench)", returnByValue: true });
    const v = r.result?.result?.value;
    if (v && v !== "undefined") { result = v; break; }
  }
  await send("Timeline.stop");
  await new Promise((r) => setTimeout(r, 3000));
  records.forEach((r) => walk(r, 0));
  console.log(result);
  console.log("records", records.length);
  console.log(Object.entries(totals).sort((a, b) => b[1] - a[1]).slice(0, 15).map(([k, v]) => `${k}: ${(v * 1000).toFixed(0)} ms`).join("\n"));
  const fn = {};
  const walk2 = (r) => { if (r.type === "FunctionCall" || r.type === "TimerFire" || r.type === "EventDispatch" || r.type === "FireAnimationFrame") { const d = r.data || {}; const k = r.type + " " + (d.type || d.scriptName || d.url || "") + ":" + (d.scriptLine || d.lineNumber || ""); fn[k] = (fn[k] || 0) + ((r.endTime ?? r.startTime) - r.startTime); } (r.children || []).forEach(walk2); };
  records.forEach(walk2);
  const walkTop = {};
  const tw = (r, path) => { const dur = ((r.endTime ?? r.startTime) - r.startTime) * 1000; if (dur > 30) { const k = path + ">" + r.type + (r.data?.scriptName ? "@" + r.data.scriptName.split("/").pop() + ":" + r.data.scriptColumn : "") + (r.data?.type ? "(" + r.data.type + ")" : ""); walkTop[k] = (walkTop[k] || 0) + dur; } (r.children || []).forEach((c) => tw(c, path + ">" + r.type)); };
  records.forEach((r) => tw(r, ""));
  console.log("--- slow nested (>30ms)");
  console.log(Object.entries(walkTop).sort((a, b) => b[1] - a[1]).slice(0, 14).map(([k, v]) => `${v.toFixed(0)} ms  ${k.slice(-140)}`).join("\n"));
  console.log("--- top script entries");
  console.log(Object.entries(fn).sort((a, b) => b[1] - a[1]).slice(0, 10).map(([k, v]) => `${(v * 1000).toFixed(0)} ms  ${k.slice(0, 150)}`).join("\n"));
  process.exit(0);
};
