async () => {
  const intervals = [];
  await new Promise(resolve => { let last = performance.now(); const end = last + 5000; const f = now => { intervals.push(now - last); last = now; if (now < end) requestAnimationFrame(f); else resolve(); }; requestAnimationFrame(f); });
  intervals.shift();
  const total = intervals.reduce((a, b) => a + b, 0);
  const sorted = [...intervals].sort((a, b) => a - b);
  return { fps: +(intervals.length / (total / 1000)).toFixed(1), p50: +sorted[Math.floor(sorted.length / 2)].toFixed(1), p95: +sorted[Math.floor(sorted.length * 0.95)].toFixed(1), worst: Math.round(sorted[sorted.length - 1]), over20ms: intervals.filter(t => t > 20).length };
}
