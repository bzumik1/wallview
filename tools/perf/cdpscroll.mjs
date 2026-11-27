const [port, path] = process.argv.slice(2);
const targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
const page = targets.find((t) => t.type === "page");
const ws = new WebSocket(page.webSocketDebuggerUrl);
let id = 0; const pending = new Map(); const events = []; let done = null;
const send = (method, params = {}) => new Promise((resolve) => { const i = ++id; pending.set(i, resolve); ws.send(JSON.stringify({ id: i, method, params })); });
ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } if (m.method === "Tracing.dataCollected") events.push(...m.params.value); if (m.method === "Tracing.tracingComplete") done(); };
ws.onopen = async () => {
  if (path) {
    await send("Runtime.evaluate", { expression: `history.pushState(null, "", "${path}"); window.dispatchEvent(new CustomEvent("location-changed")); 1` });
    await new Promise((r) => setTimeout(r, 8000));
  }
  await send("Runtime.evaluate", { expression: "document.scrollingElement.scrollTop = 0; 1" });
  await new Promise((r) => setTimeout(r, 1000));
  await send("Tracing.start", { traceConfig: { includedCategories: ["viz", "benchmark", "cc", "disabled-by-default-devtools.timeline.frame"] }, transferMode: "ReportEvents" });
  await new Promise((r) => setTimeout(r, 500));
  const t0 = Date.now();
  await send("Input.synthesizeScrollGesture", { x: 240, y: 360, yDistance: -500, speed: 700, gestureSourceType: "touch", repeatCount: 3, repeatDelayMs: 50 });
  await send("Input.synthesizeScrollGesture", { x: 240, y: 120, yDistance: 500, speed: 700, gestureSourceType: "touch", repeatCount: 3, repeatDelayMs: 50 });
  const gestureMs = Date.now() - t0;
  const traceDone = new Promise((r) => (done = r));
  await send("Tracing.end");
  await traceDone;
  const swaps = events.filter((e) => e.name === "Display::DrawAndSwap" && e.ph === "X" || e.name === "Display::DrawAndSwap" && e.ph === "B").map((e) => e.ts).sort((a, b) => a - b);
  const intervals = swaps.slice(1).map((t, i) => (t - swaps[i]) / 1000);
  const busy = intervals.filter((x) => x < 200);
  const sorted = [...busy].sort((a, b) => a - b);
  const missed = busy.reduce((a, x) => a + Math.max(0, Math.round(x / 16.67) - 1), 0);
  const names = {}; for (const e of events) names[e.name] = (names[e.name] || 0) + 1;
  console.log(JSON.stringify({ view: path || "current", gestureMs, swaps: swaps.length, fps: +(busy.length / (busy.reduce((a, b) => a + b, 0) / 1000)).toFixed(1), p50: sorted[Math.floor(sorted.length / 2)]?.toFixed(1), p95: sorted[Math.floor(sorted.length * 0.95)]?.toFixed(1), worst: sorted[sorted.length - 1]?.toFixed(1), missedFrames: missed, topEvents: Object.entries(names).sort((a, b) => b[1] - a[1]).slice(0, 6).map(([k, v]) => k + ":" + v).join(" ") }));
  process.exit(0);
};
