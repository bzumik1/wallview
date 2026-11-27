async () => {
  const deep = (root, out = []) => { for (const el of root.querySelectorAll("*")) { out.push(el); if (el.shadowRoot) deep(el.shadowRoot, out); } return out; };
  const find = () => {
    if (document.scrollingElement.scrollHeight > innerHeight + 200) return document.scrollingElement;
    return deep(document).filter(el => { const s = getComputedStyle(el); return /(auto|scroll)/.test(s.overflowY) && el.scrollHeight > el.clientHeight + 200; }).sort((a, b) => b.scrollHeight - a.scrollHeight)[0];
  };
  let scroller;
  for (let i = 0; i < 120 && !(scroller = find()); i++) await new Promise(r => setTimeout(r, 500));
  if (!scroller) return { error: "no scroller", elements: deep(document).length };
  const loaded = Math.round(performance.now() / 1000);
  await new Promise(r => setTimeout(r, 3000));
  const max = scroller.scrollHeight - scroller.clientHeight;
  scroller.scrollTop = 0;
  await new Promise(r => setTimeout(r, 500));
  const times = [];
  let last = performance.now(), pos = 0, dir = 1;
  const end = last + 4000;
  await new Promise(resolve => {
    const step = (now) => {
      times.push(now - last); last = now;
      pos += dir * 6; if (pos >= max) { pos = max; dir = -1; } if (pos <= 0) { pos = 0; dir = 1; }
      scroller.scrollTop = pos;
      if (now < end) requestAnimationFrame(step); else resolve();
    };
    requestAnimationFrame(step);
  });
  times.shift();
  const total = times.reduce((a, b) => a + b, 0);
  const long = times.filter(t => t > 20).length;
  return { loaded, scroller: scroller.tagName + (scroller.id ? "#" + scroller.id : ""), max, frames: times.length, fps: +(times.length / (total / 1000)).toFixed(1), jank: +(100 * long / times.length).toFixed(0) + "%", worst: Math.round(Math.max(...times)), elements: deep(document).length };
}
