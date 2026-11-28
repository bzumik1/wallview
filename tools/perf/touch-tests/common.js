(function () {
	const log = [];
	window.__touchLog = log;
	const frames = [];
	let active = 0;
	let long = 0;
	let worst = 0;
	let prev = 0;
	let activeFrames = 0;
	let downs = 0;
	let moves = 0;
	const moveTimes = [];
	const extra = new Map();
	const bar = document.createElement("div");
	bar.id = "tt-bar";
	bar.innerHTML = '<a href="index.html">◀ Testy</a><span class="title"></span><button type="button">Reset</button>';
	bar.querySelector(".title").textContent = document.title;
	const stats = document.createElement("div");
	stats.id = "tt-stats";
	document.addEventListener("DOMContentLoaded", () => {
		document.body.prepend(bar);
		document.body.append(stats);
	});
	const resetHandlers = [];
	function reset() {
		long = 0;
		worst = 0;
		activeFrames = 0;
		downs = 0;
		moves = 0;
		extra.clear();
		for (const f of resetHandlers) {
			f();
		}
	}
	bar.querySelector("button").addEventListener("click", reset);
	addEventListener("pointerdown", (e) => {
		downs++;
		active = performance.now();
	}, { capture: true, passive: true });
	addEventListener("pointermove", (e) => {
		moves++;
		moveTimes.push(e.timeStamp);
		active = performance.now();
	}, { capture: true, passive: true });
	addEventListener("scroll", () => {
		active = performance.now();
	}, { capture: true, passive: true });
	function tick(now) {
		frames.push(now);
		while (frames.length && now - frames[0] > 1000) {
			frames.shift();
		}
		while (moveTimes.length && now - moveTimes[0] > 1000) {
			moveTimes.shift();
		}
		if (prev && now - active < 300) {
			const d = now - prev;
			activeFrames++;
			if (d > 25) {
				long++;
			}
			worst = Math.max(worst, d);
		}
		prev = now;
		let text = `fps ${frames.length}  moves/s ${moveTimes.length}\n` +
			`active frames ${activeFrames}  >25ms ${long}  worst ${worst.toFixed(0)} ms`;
		for (const [k, v] of extra) {
			text += `\n${k} ${v}`;
		}
		stats.textContent = text;
		requestAnimationFrame(tick);
	}
	requestAnimationFrame(tick);
	window.tt = {
		set(key, value) {
			extra.set(key, value);
		},
		record(kind, data) {
			const entry = Object.assign({ kind, t: Math.round(performance.now()), longFrames: long, worstFrame: Math.round(worst) }, data);
			log.push(entry);
			return entry;
		},
		onReset(f) {
			resetHandlers.push(f);
		},
		frames() {
			return { activeFrames, long, worst };
		},
	};
})();
