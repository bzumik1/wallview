function scrollMeter(el, axis, name) {
	const pos = () => axis === "x" ? el.scrollLeft : el.scrollTop;
	let touching = false;
	let lastPos = pos();
	let lastChange = 0;
	let lastMove = 0;
	let fling = null;
	let catchAt = 0;
	let catchPos = 0;
	let stuck = 0;
	let jumps = 0;
	let flings = 0;
	let caught = 0;
	let catchTries = 0;
	let lastFling = "";
	const deltas = [];
	function show() {
		tt.set(`${name} drag`, `stuck ${stuck}  jumps ${jumps}`);
		tt.set(`${name} fling`, `${flings}  ${lastFling}`);
		tt.set(`${name} catch`, `${caught}/${catchTries}`);
	}
	tt.onReset(() => {
		stuck = jumps = flings = caught = catchTries = 0;
		lastFling = "";
		show();
	});
	el.addEventListener("touchstart", () => {
		const now = performance.now();
		if (!touching && now - lastChange < 60) {
			catchAt = now;
			catchPos = pos();
		}
		touching = true;
		fling = null;
	}, { passive: true });
	el.addEventListener("touchmove", () => {
		lastMove = performance.now();
	}, { passive: true });
	const end = () => {
		touching = false;
		fling = { t: performance.now(), start: pos(), first: null, peak: 0 };
	};
	el.addEventListener("touchend", end, { passive: true });
	el.addEventListener("touchcancel", end, { passive: true });
	function tick(now) {
		const p = pos();
		const d = p - lastPos;
		if (d !== 0) {
			lastChange = now;
		}
		if (touching && now - lastMove < 20) {
			if (d === 0) {
				stuck++;
			} else {
				const s = [...deltas].map(Math.abs).sort((a, b) => a - b);
				const med = s.length ? s[Math.floor(s.length / 2)] : 0;
				if (med > 0 && Math.abs(d) > 2.5 * med && Math.abs(d) > 12) {
					jumps++;
				}
				deltas.push(d);
				if (deltas.length > 12) {
					deltas.shift();
				}
			}
		}
		if (catchAt && now - catchAt > 120) {
			catchTries++;
			const moved = Math.abs(pos() - catchPos);
			if (moved < 8) {
				caught++;
			}
			tt.record(`${name}-catch`, { moved: Math.round(moved) });
			catchAt = 0;
		}
		if (fling) {
			if (d !== 0 && fling.first === null) {
				fling.first = now - fling.t;
			}
			fling.peak = Math.max(fling.peak, Math.abs(d));
			if (now - lastChange > 120 && now - fling.t > 120) {
				const dist = Math.abs(p - fling.start);
				if (dist > 20) {
					flings++;
					lastFling = `${Math.round(dist)} px, start +${fling.first === null ? "-" : fling.first.toFixed(0)} ms, peak ${fling.peak.toFixed(0)} px/frame`;
					tt.record(`${name}-fling`, { distance: Math.round(dist), startDelay: fling.first === null ? null : Math.round(fling.first), peak: Math.round(fling.peak), duration: Math.round(lastChange - fling.t) });
				}
				fling = null;
			}
		}
		lastPos = p;
		show();
		requestAnimationFrame(tick);
	}
	requestAnimationFrame(tick);
}
