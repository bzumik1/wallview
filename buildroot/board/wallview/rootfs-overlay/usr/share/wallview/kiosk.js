(() => {
  "use strict";

  if (window.__wallviewKiosk) {
    return;
  }
  window.__wallviewKiosk = true;

  const EVENT_INTERVAL_MS = 1000;
  const INTERACTION_GRACE_MS = 600;
  const URGENT = /"type":"(result|pong|auth[a-z_]*)"/;
  const NativeWebSocket = window.WebSocket;
  const sockets = new Set();
  let touching = false;
  let interactionEnd = 0;

  const interacting = () => touching || performance.now() - interactionEnd < INTERACTION_GRACE_MS;

  const startInteraction = () => {
    touching = true;
  };

  const endInteraction = () => {
    touching = false;
    interactionEnd = performance.now();
    setTimeout(() => {
      for (const socket of sockets) {
        socket._wallviewSchedule();
      }
    }, INTERACTION_GRACE_MS);
  };

  for (const type of ["touchstart", "pointerdown"]) {
    window.addEventListener(type, startInteraction, { capture: true, passive: true });
  }
  for (const type of ["touchend", "touchcancel", "pointerup", "pointercancel"]) {
    window.addEventListener(type, endInteraction, { capture: true, passive: true });
  }

  const ENTITY_KEYS = new Set(["a", "c", "r"]);

  const isEntityEvent = (message) =>
    message.type === "event" &&
    message.event !== null &&
    typeof message.event === "object" &&
    !Array.isArray(message.event) &&
    Object.keys(message.event).every((key) => ENTITY_KEYS.has(key));

  const contextObject = (context) =>
    typeof context === "string" ? { id: context } : context || {};

  const fullContext = (context) =>
    typeof context === "string" ? { id: context, parent_id: null, user_id: null } : context;

  const applyDiff = (full, diff) => {
    const plus = diff["+"] || {};
    const minus = diff["-"] || {};
    const result = { ...full };
    if (plus.s !== undefined) {
      result.s = plus.s;
    }
    if (plus.c) {
      result.c = { ...fullContext(result.c), ...contextObject(plus.c) };
    }
    if (plus.lc !== undefined) {
      result.lc = plus.lc;
      delete result.lu;
    } else if (plus.lu !== undefined) {
      result.lu = plus.lu;
    }
    if (plus.a || minus.a) {
      result.a = { ...result.a, ...plus.a };
      for (const key of minus.a || []) {
        delete result.a[key];
      }
    }
    return result;
  };

  const combineDiffs = (first, second) => {
    const plusFirst = first["+"] || {};
    const plusSecond = second["+"] || {};
    if (plusFirst.lc !== undefined && plusSecond.lc === undefined && plusSecond.lu !== undefined) {
      return null;
    }
    const plus = { ...plusFirst };
    for (const [key, value] of Object.entries(plusSecond)) {
      if (key !== "a" && key !== "c") {
        plus[key] = value;
      }
    }
    if (plusSecond.c) {
      plus.c = plusFirst.c ? { ...contextObject(plusFirst.c), ...contextObject(plusSecond.c) } : plusSecond.c;
    }
    if (plusSecond.lc !== undefined) {
      delete plus.lu;
    }
    const removed = new Set((first["-"] || {}).a || []);
    const attributes = { ...plusFirst.a };
    for (const [key, value] of Object.entries(plusSecond.a || {})) {
      attributes[key] = value;
      removed.delete(key);
    }
    for (const key of (second["-"] || {}).a || []) {
      delete attributes[key];
      removed.add(key);
    }
    delete plus.a;
    if (Object.keys(attributes).length > 0) {
      plus.a = attributes;
    }
    const combined = {};
    if (Object.keys(plus).length > 0) {
      combined["+"] = plus;
    }
    if (removed.size > 0) {
      combined["-"] = { a: [...removed] };
    }
    return combined;
  };

  const addToMerged = (merged, event) => {
    for (const [entityId, state] of Object.entries(event.a || {})) {
      merged.a[entityId] = state;
      delete merged.c[entityId];
      merged.r.delete(entityId);
    }
    for (const entityId of event.r || []) {
      delete merged.a[entityId];
      delete merged.c[entityId];
      merged.r.add(entityId);
    }
    for (const [entityId, diff] of Object.entries(event.c || {})) {
      if (merged.a[entityId]) {
        merged.a[entityId] = applyDiff(merged.a[entityId], diff);
      } else if (merged.c[entityId]) {
        const combined = combineDiffs(merged.c[entityId], diff);
        if (combined === null) {
          return false;
        }
        merged.c[entityId] = combined;
      } else {
        merged.c[entityId] = diff;
      }
    }
    return true;
  };

  const toEvent = (merged) => {
    const event = {};
    if (Object.keys(merged.a).length > 0) {
      event.a = merged.a;
    }
    if (merged.r.size > 0) {
      event.r = [...merged.r];
    }
    if (Object.keys(merged.c).length > 0) {
      event.c = merged.c;
    }
    return { id: merged.id, type: "event", event };
  };

  const emptyMerged = (id) => ({ id, a: {}, c: {}, r: new Set() });

  const coalesce = (rawMessages) => {
    const output = [];
    const open = new Map();
    for (const raw of rawMessages) {
      const parsed = JSON.parse(raw);
      for (const message of Array.isArray(parsed) ? parsed : [parsed]) {
        if (!isEntityEvent(message)) {
          output.push(message);
          continue;
        }
        let merged = open.get(message.id);
        if (!merged) {
          merged = emptyMerged(message.id);
          open.set(message.id, merged);
          output.push(merged);
        }
        const snapshot = { a: { ...merged.a }, c: { ...merged.c }, r: new Set(merged.r) };
        if (!addToMerged(merged, message.event)) {
          Object.assign(merged, snapshot);
          const next = emptyMerged(message.id);
          addToMerged(next, message.event);
          open.set(message.id, next);
          output.push(next);
        }
      }
    }
    return output.map((item) => (item.r instanceof Set ? toEvent(item) : item));
  };

  class WallViewWebSocket extends NativeWebSocket {
    constructor(url, protocols) {
      super(url, protocols);
      if (!new URL(url, location.href).pathname.endsWith("/api/websocket")) {
        return;
      }
      this._wallviewListeners = [];
      this._wallviewOnMessage = null;
      this._wallviewQueue = [];
      this._wallviewTimer = null;
      this._wallviewLastDelivery = 0;
      sockets.add(this);
      super.addEventListener("close", () => sockets.delete(this));
      super.addEventListener("message", (event) => this._wallviewReceive(event));
    }

    _wallviewReceive(event) {
      if (typeof event.data !== "string") {
        this._wallviewDispatch(event);
        return;
      }
      this._wallviewQueue.push(event.data);
      if (URGENT.test(event.data)) {
        this._wallviewFlush();
      } else {
        this._wallviewSchedule();
      }
    }

    _wallviewSchedule() {
      if (this._wallviewQueue.length === 0 || this._wallviewTimer !== null || interacting()) {
        return;
      }
      const wait = EVENT_INTERVAL_MS - (performance.now() - this._wallviewLastDelivery);
      if (wait <= 0) {
        this._wallviewFlush();
      } else {
        this._wallviewTimer = setTimeout(() => {
          this._wallviewTimer = null;
          if (interacting()) {
            return;
          }
          this._wallviewFlush();
        }, wait);
      }
    }

    _wallviewFlush() {
      if (this._wallviewTimer !== null) {
        clearTimeout(this._wallviewTimer);
        this._wallviewTimer = null;
      }
      const queue = this._wallviewQueue;
      if (queue.length === 0) {
        return;
      }
      this._wallviewQueue = [];
      this._wallviewLastDelivery = performance.now();
      let data;
      try {
        data = queue.length === 1 ? queue[0] : JSON.stringify(coalesce(queue));
      } catch (error) {
        data = `[${queue.map((raw) => (raw.startsWith("[") ? raw.slice(1, -1) : raw)).join(",")}]`;
      }
      this._wallviewDispatch(new MessageEvent("message", { data }));
    }

    _wallviewDispatch(event) {
      for (const listener of [...this._wallviewListeners]) {
        if (typeof listener === "function") {
          listener.call(this, event);
        } else if (listener && typeof listener.handleEvent === "function") {
          listener.handleEvent(event);
        }
      }
      if (typeof this._wallviewOnMessage === "function") {
        this._wallviewOnMessage.call(this, event);
      }
    }

    addEventListener(type, listener, options) {
      if (type === "message" && this._wallviewListeners) {
        if (listener && !this._wallviewListeners.includes(listener)) {
          this._wallviewListeners.push(listener);
        }
        return;
      }
      super.addEventListener(type, listener, options);
    }

    removeEventListener(type, listener, options) {
      if (type === "message" && this._wallviewListeners) {
        this._wallviewListeners = this._wallviewListeners.filter((item) => item !== listener);
        return;
      }
      super.removeEventListener(type, listener, options);
    }

    get onmessage() {
      return this._wallviewListeners ? this._wallviewOnMessage : super.onmessage;
    }

    set onmessage(handler) {
      if (this._wallviewListeners) {
        this._wallviewOnMessage = handler;
      } else {
        super.onmessage = handler;
      }
    }
  }

  window.WebSocket = WallViewWebSocket;

  const SHADOW_STYLES = {
    "ha-spinner": `
      .indicator {
        animation: none !important;
        stroke-dasharray: calc(0.6 * var(--circumference)), calc(0.8 * var(--circumference)) !important;
        stroke-dashoffset: 0 !important;
      }
    `,
  };

  const nativeAttachShadow = Element.prototype.attachShadow;
  Element.prototype.attachShadow = function attachShadow(init) {
    const root = nativeAttachShadow.call(this, init);
    const css = SHADOW_STYLES[this.localName];
    if (css) {
      const style = document.createElement("style");
      style.textContent = css;
      root.appendChild(style);
    }
    return root;
  };
})();
