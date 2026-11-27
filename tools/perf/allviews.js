async () => {
  const deep = (root, out = []) => { for (const el of root.querySelectorAll("*")) { out.push(el); if (el.shadowRoot) deep(el.shadowRoot, out); } return out; };
  const frames = (ms, onFrame) => new Promise(resolve => { const t = []; let last = performance.now(); const end = last + ms; const f = now => { t.push(now - last); last = now; if (onFrame) onFrame(); if (now < end) requestAnimationFrame(f); else { t.shift(); const s = [...t].sort((a, b) => a - b); resolve({ fps: +(t.length / (t.reduce((a, b) => a + b, 0) / 1000)).toFixed(1), p50: Math.round(s[Math.floor(s.length / 2)]), p95: Math.round(s[Math.floor(s.length * 0.95)]), worst: Math.round(s[s.length - 1]), over20: t.filter(x => x > 20).length, n: t.length }); } }; requestAnimationFrame(f); });
  const ha = document.querySelector("home-assistant");
  const panel = ha.hass.panelUrl;
  const root = deep(document).find(e => e.tagName === "HUI-ROOT");
  const views = root?.lovelace?.config?.views || [];
  const results = [];
  for (let i = 0; i < views.length; i++) {
    const v = views[i];
    history.pushState(null, "", `/${panel}/${v.path ?? i}`);
    window.dispatchEvent(new CustomEvent("location-changed"));
    await new Promise(r => setTimeout(r, 7000));
    const all = deep(document);
    const animations = [];
    for (const el of all) for (const a of (el.getAnimations ? el.getAnimations() : [])) animations.push((el.getRootNode()?.host?.localName || el.localName) + ":" + (a.animationName || "transition"));
    const idle = await frames(4000);
    const scroller = document.scrollingElement;
    const max = scroller.scrollHeight - scroller.clientHeight;
    let pos = 0, dir = 1;
    const scroll = max > 50 ? await frames(4000, () => { pos += dir * 6; if (pos >= max) { pos = max; dir = -1; } if (pos <= 0) { pos = 0; dir = 1; } scroller.scrollTop = pos; }) : null;
    scroller.scrollTop = 0;
    results.push({ view: v.path ?? i, elements: all.length, animations: [...new Set(animations)].join(" ") || "-", idle, scroll });
  }
  return results;
}
