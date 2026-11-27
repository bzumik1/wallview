import { writeFileSync } from "node:fs";
const [host, out] = process.argv.slice(2);
const ws = new WebSocket(`ws://${host}/socket/1/1/WebPage`);
let id = 0, targetId = null; const pending = new Map();
function send(method, params = {}) { const msgId = ++id; const message = { id: msgId, method, params }; return new Promise((resolve) => { pending.set(msgId, resolve); if (targetId) ws.send(JSON.stringify({ id: ++id, method: "Target.sendMessageToTarget", params: { targetId, message: JSON.stringify(message) } })); else ws.send(JSON.stringify(message)); }); }
function handle(m) { if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } }
ws.onmessage = (e) => { const d = JSON.parse(e.data); if (d.method === "Target.targetCreated" && !targetId) targetId = d.params.targetInfo.targetId; if (d.method === "Target.dispatchMessageFromTarget") return handle(JSON.parse(d.params.message)); handle(d); };
ws.onopen = async () => {
  await new Promise((r) => setTimeout(r, 500));
  await send("Page.enable");
  const size = await send("Runtime.evaluate", { expression: "JSON.stringify([innerWidth, innerHeight])", returnByValue: true });
  const [w, h] = JSON.parse(size.result.result.value);
  const shot = await send("Page.snapshotRect", { x: 0, y: 0, width: w, height: h, coordinateSystem: "Viewport" });
  const url = shot.result?.dataURL;
  if (!url) { console.log(JSON.stringify(shot)); process.exit(1); }
  writeFileSync(out, Buffer.from(url.split(",")[1], "base64"));
  console.log(out, w, h);
  process.exit(0);
};
