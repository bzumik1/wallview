const host = process.argv[2];
const ws = new WebSocket(`ws://${host}/socket/1/1/WebPage`);
let id = 0, targetId = null; const pending = new Map();
function send(method, params = {}) { const msgId = ++id; const message = { id: msgId, method, params }; return new Promise((resolve) => { pending.set(msgId, resolve); if (targetId) ws.send(JSON.stringify({ id: ++id, method: "Target.sendMessageToTarget", params: { targetId, message: JSON.stringify(message) } })); else ws.send(JSON.stringify(message)); }); }
function handle(m) { if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } }
ws.onmessage = (e) => { const d = JSON.parse(e.data); if (d.method === "Target.targetCreated" && !targetId) targetId = d.params.targetInfo.targetId; if (d.method === "Target.dispatchMessageFromTarget") return handle(JSON.parse(d.params.message)); handle(d); };
ws.onopen = async () => {
  await new Promise((r) => setTimeout(r, 500));
  await send("DOM.enable").catch(() => {});
  const doc = await send("DOM.getDocument");
  await send("LayerTree.enable");
  const lt = await send("LayerTree.layersForNode", { nodeId: doc.result.root.nodeId });
  const layers = lt.result?.layers || [];
  let area = 0; const reasonCount = {};
  for (const l of layers) {
    area += l.bounds.width * l.bounds.height;
    const r = await send("LayerTree.reasonsForCompositingLayer", { layerId: l.layerId });
    const reasons = Object.keys(r.result?.compositingReasons || {}).filter((k) => r.result.compositingReasons[k]);
    for (const k of reasons) reasonCount[k] = (reasonCount[k] || 0) + 1;
  }
  console.log(JSON.stringify({ layers: layers.length, areaScreens: +(area / (480 * 480)).toFixed(1), memory: layers.reduce((a, l) => a + (l.memory || 0), 0), reasons: reasonCount }));
  if (lt.error) console.log(JSON.stringify(lt.error));
  process.exit(0);
};
