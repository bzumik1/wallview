const host = process.argv[2];
const expression = process.argv[3];
const ws = new WebSocket(`ws://${host}/socket/1/1/WebPage`);
let id = 0;
const pending = new Map();
let targetId = null;
function send(method, params = {}) {
  const msgId = ++id;
  const message = { id: msgId, method, params };
  return new Promise((resolve) => {
    pending.set(msgId, resolve);
    if (targetId) {
      const outerId = ++id;
      ws.send(JSON.stringify({ id: outerId, method: "Target.sendMessageToTarget", params: { targetId, message: JSON.stringify(message) } }));
    } else {
      ws.send(JSON.stringify(message));
    }
  });
}
ws.onmessage = (event) => {
  const data = JSON.parse(event.data);
  if (data.method === "Target.targetCreated" && !targetId) targetId = data.params.targetInfo.targetId;
  if (data.method === "Target.dispatchMessageFromTarget") {
    const inner = JSON.parse(data.params.message);
    if (inner.id && pending.has(inner.id)) { pending.get(inner.id)(inner); pending.delete(inner.id); }
    return;
  }
  if (data.id && pending.has(data.id)) { pending.get(data.id)(data); pending.delete(data.id); }
};
ws.onopen = async () => {
  await new Promise((r) => setTimeout(r, 500));
  const start = await send("Runtime.evaluate", { expression: `window.__bench = undefined; (${expression})().then(r => window.__bench = r, e => window.__bench = {error: String(e)}); "started"`, returnByValue: true });
  if (start.error || start.result?.wasThrown) { console.log(JSON.stringify(start)); process.exit(1); }
  for (let i = 0; i < 600; i++) {
    await new Promise((r) => setTimeout(r, 500));
    const r = await send("Runtime.evaluate", { expression: "JSON.stringify(window.__bench)", returnByValue: true });
    const value = r.result?.result?.value;
    if (value && value !== "undefined") { console.log(value); process.exit(0); }
  }
  console.log("timeout"); process.exit(1);
};
ws.onerror = (e) => { console.log("ws error", e.message); process.exit(1); };
