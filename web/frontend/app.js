// Market Data Simulator - web frontend (no build step, no dependencies).
import { BarChart, DepthChart, LineChart, fmtNum } from "./charts.js";

// =============================================================================
// Utilities
// =============================================================================

/** Tiny DOM builder. Text is always set via textContent (never innerHTML), so
 *  values coming from the server can't inject markup. */
function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k === "class") el.className = v;
    else if (k === "text") el.textContent = v;
    else if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
    else if (k === "dataset") Object.assign(el.dataset, v);
    else if (k === "value") el.value = v;
    else if (k === "checked") el.checked = v;
    else el.setAttribute(k, v === true ? "" : v);
  }
  for (const c of children.flat()) {
    if (c === null || c === undefined || c === false) continue;
    el.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return el;
}
const $ = (sel, root = document) => root.querySelector(sel);
const tip = (text) => h("span", { class: "tip", tabindex: "0", "data-tip": text, "aria-label": text }, "?");

const fmt = {
  money: (v) => (v === undefined || v === null || !isFinite(v) ? "–" : (v < 0 ? "−$" : "$") + Math.abs(v).toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 })),
  signedMoney: (v) => (!isFinite(v) ? "–" : (v > 0 ? "+" : v < 0 ? "−" : "") + "$" + Math.abs(v).toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 })),
  int: (v) => (v === undefined || v === null || !isFinite(v) ? "–" : Math.round(v).toLocaleString()),
  px: (v) => (v === undefined || v === null || !isFinite(v) ? "–" : v.toFixed(2)),
  pct: (v) => (!isFinite(v) ? "–" : (v * 100).toFixed(1) + "%"),
  rate: (v) => (!isFinite(v) ? "–" : fmtNum(v, 2) + "/s"),
  us: (v) => (!isFinite(v) ? "–" : v < 1000 ? v.toFixed(1) + " µs" : (v / 1000).toFixed(2) + " ms"),
  ns: (v) => (!isFinite(v) ? "–" : v < 1000 ? v.toFixed(0) + " ns" : v < 1e6 ? (v / 1e3).toFixed(1) + " µs" : (v / 1e6).toFixed(2) + " ms"),
  dur: (s) => (!isFinite(s) ? "–" : s === 0 ? "0s" : s < 1 ? (s * 1000).toFixed(s < 0.01 ? 2 : 0) + " ms" : s < 60 ? s.toFixed(1) + "s" : Math.floor(s / 60) + "m " + Math.round(s % 60) + "s"),
  when: (t) => new Date(t * 1000).toLocaleString(undefined, { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit" }),
  clock: (ms) => new Date(ms).toLocaleTimeString(undefined, { hour12: false }),
  perShare: (v) => (!isFinite(v) ? "–" : (v > 0 ? "+" : v < 0 ? "−" : "") + "$" + Math.abs(v).toFixed(4)),
  sign: (v) => (v > 0 ? "pos" : v < 0 ? "neg" : ""),
};

function toast(message, kind = "") {
  const el = h("div", { class: `toast ${kind}`, role: "status" }, message);
  $("#toasts").append(el);
  setTimeout(() => el.remove(), kind === "err" ? 7000 : 4000);
}

// ---- API client ---------------------------------------------------------------
const TOKEN_KEY = "mds-token";
function getToken() { try { return localStorage.getItem(TOKEN_KEY) || ""; } catch { return ""; } }
function setToken(t) { try { t ? localStorage.setItem(TOKEN_KEY, t) : localStorage.removeItem(TOKEN_KEY); } catch { /* private mode */ } }

class ApiError extends Error {
  constructor(message, status, details) { super(message); this.status = status; this.details = details; }
}

async function api(path, { method = "GET", body } = {}) {
  const headers = {};
  if (body !== undefined) headers["Content-Type"] = "application/json";
  const token = getToken();
  if (token && method !== "GET") headers["Authorization"] = `Bearer ${token}`;
  let res;
  try {
    res = await fetch(path, { method, headers, body: body === undefined ? undefined : JSON.stringify(body) });
  } catch {
    throw new ApiError("Cannot reach the server. Check your connection.", 0);
  }
  const data = await res.json().catch(() => ({}));
  if (!res.ok) {
    if (res.status === 401) openTokenDialog();
    throw new ApiError(data.error || `Request failed (${res.status})`, res.status, data.details);
  }
  return data;
}

function reportError(e, form) {
  if (e.details && form) {
    for (const [name, msg] of Object.entries(e.details)) {
      const input = form.querySelector(`[name="${CSS.escape(name)}"]`);
      if (input) {
        input.classList.add("invalid");
        const field = input.closest(".field");
        if (field && !field.querySelector(".error")) field.append(h("div", { class: "error", text: msg }));
      }
    }
  }
  toast(e.message + (e.details ? ": " + Object.entries(e.details).map(([k, v]) => `${k} ${v}`).join(", ") : ""), "err");
}

// Polls a job until it finishes, calling onUpdate with each snapshot.
function watchJob(id, onUpdate) {
  let stopped = false, timer = null;
  const tick = async () => {
    if (stopped) return;
    try {
      const job = await api(`/api/jobs/${id}`);
      onUpdate(job);
      if (["queued", "running"].includes(job.status)) timer = setTimeout(tick, 900);
    } catch (e) {
      if (!stopped) timer = setTimeout(tick, 2500);
    }
  };
  tick();
  return () => { stopped = true; clearTimeout(timer); };
}

// =============================================================================
// Global state: server info + live stream (shared by every page)
// =============================================================================

const state = { info: null, cleanup: [] };

async function loadInfo() {
  try {
    state.info = await api("/api/info");
  } catch (e) {
    state.info = null;
  }
  return state.info;
}

const live = {
  es: null,
  status: null,
  config: null,
  tick: null,
  points: [],   // [ms, mid, pnl, position]
  trades: [],   // newest first: [ts, px, qty, side]
  fills: [],    // newest first: [ts, side, px, qty, maker]
  listeners: new Set(),
  connected: false,

  connect() {
    if (this.es) return;
    this.es = new EventSource("/api/live/stream");
    this.es.addEventListener("open", () => { this.connected = true; this.emit("conn"); });
    this.es.addEventListener("error", () => { this.connected = false; this.emit("conn"); });
    this.es.addEventListener("status", (e) => { this.status = JSON.parse(e.data); this.emit("status"); });
    this.es.addEventListener("hello", (e) => {
      const d = JSON.parse(e.data);
      this.config = d.config;
      this.points = []; this.trades = []; this.fills = [];
      this.emit("hello");
    });
    this.es.addEventListener("history", (e) => {
      const d = JSON.parse(e.data);
      this.points = d.points || [];
      this.fills = (d.fills || []).slice().reverse();
      this.emit("history");
    });
    this.es.addEventListener("tick", (e) => {
      const t = JSON.parse(e.data);
      this.tick = t;
      if (t.book && t.book.mid !== undefined) {
        this.points.push([t.sim_ts / 1e6, t.book.mid, t.strategy.pnl, t.strategy.position]);
        if (this.points.length > 1200) this.points.splice(0, this.points.length - 1200);
      }
      for (const tr of t.trades || []) this.trades.unshift(tr);
      this.trades.length = Math.min(this.trades.length, 60);
      for (const f of t.strategy.fills || []) this.fills.unshift(f);
      this.fills.length = Math.min(this.fills.length, 60);
      if (this.status && this.status.state !== "running") { this.status.state = "running"; this.emit("status"); }
      this.emit("tick");
    });
  },
  on(fn) { this.listeners.add(fn); return () => this.listeners.delete(fn); },
  emit(kind) { for (const fn of this.listeners) fn(kind); },
};

function updateConnPill() {
  const pill = $("#conn");
  const st = live.status ? live.status.state : null;
  let cls = "pill", text = "connecting";
  if (!live.connected) { cls += " err"; text = "offline"; }
  else if (st === "running") { cls += " ok"; text = "market live"; }
  else if (st === "paused") { cls += " warn"; text = "paused (benchmark)"; }
  else if (st === "restarting") { cls += " warn"; text = "restarting"; }
  else if (st === "stopped") { cls += " warn"; text = "market stopped"; }
  pill.className = cls;
  pill.lastChild.textContent = text;
}
live.on((k) => { if (k === "conn" || k === "status") updateConnPill(); });

// =============================================================================
// Shared form helpers
// =============================================================================

const LABELS = {
  events: ["Events", "How many market events (order adds, cancels, modifies, market orders) to generate."],
  seed: ["Seed", "Random seed. The same seed reproduces exactly the same market. Leave empty for a random one."],
  rate: ["Event rate (/s)", "Average number of events per simulated second. Higher = busier market."],
  vol: ["Volatility", "How fast the hidden fair value moves, in ticks per √second. 1.5 ≈ 30% annualised for a $100 stock."],
  informed: ["Informed flow", "Share of aggressive orders that know where the price goes next. Higher = more adverse selection for market makers."],
  impact: ["Price impact", "How far each traded lot permanently moves the price (ticks). 0 = trades never move prices."],
  queue: ["Queue type", "Which inter-thread queue to run: lock-free SPSC, mutex, or both to compare them."],
  batch: ["Consumer batch", "How many events the consumer drains per pop. 1 isolates the queue cost; 64 amortises it."],
  capacity: ["Queue capacity", "Ring buffer size (rounded up to a power of two)."],
  save_events: ["Save events", "Keep the generated events so you can backtest on exactly this data later."],
  redis: ["Store in Redis", "Also write time series and the event log to the configured Redis."],
  latency_us: ["Order latency (µs)", "Delay before the strategy's orders and cancels reach the exchange."],
  maker_bps: ["Maker fee (bps)", "Fee for resting orders that get filled. Negative = rebate paid to you."],
  taker_bps: ["Taker fee (bps)", "Fee for orders that cross the spread."],
  messages: ["Raw queue messages", "Messages pushed through each queue in the raw benchmark."],
  reps: ["Repetitions", "Each configuration runs this many times; the median is reported."],
  speed: ["Speed", "Simulated seconds per real second. 1 = real time, 5 = five times faster."],
  snapshot_ms: ["Update interval (ms)", "How often the screen updates."],
};

function numberField(name, schema, value, { span } = {}) {
  const s = schema[name];
  const [label, help] = LABELS[name] || [name, ""];
  const input = h("input", {
    name, id: `f-${name}`, type: "number", step: s.type === "int" ? "1" : "any",
    min: s.min, max: s.max, value: value ?? s.default ?? "", placeholder: s.default === null ? "random" : "",
  });
  input.addEventListener("input", () => { input.classList.remove("invalid"); input.closest(".field").querySelector(".error")?.remove(); });
  return h("div", { class: "field" + (span ? " span-2" : "") },
    h("label", { for: `f-${name}` }, label, help ? tip(help) : null), input,
    s.min !== undefined ? h("div", { class: "help", text: `${fmtNum(s.min, 0)} – ${fmtNum(s.max, 0)}` }) : null);
}

function selectField(name, schema, value, labels = {}) {
  const s = schema[name];
  const [label, help] = LABELS[name] || [name, ""];
  return h("div", { class: "field" },
    h("label", { for: `f-${name}` }, label, help ? tip(help) : null),
    h("select", { name, id: `f-${name}` }, s.choices.map((c) => h("option", { value: c, selected: (value ?? s.default) === c }, labels[c] || c))));
}

function boolField(name, schema, value, disabled, note) {
  const [label, help] = LABELS[name] || [name, ""];
  return h("div", { class: "field" },
    h("label", { class: "toggle" }, h("input", { type: "checkbox", name, checked: value ?? schema[name].default, disabled }), label, help ? tip(help) : null),
    note ? h("div", { class: "help", text: note }) : null);
}

function readForm(form, schema) {
  const out = {};
  for (const [name, s] of Object.entries(schema)) {
    const el = form.elements.namedItem(name);
    if (!el) continue;
    if (s.type === "bool") out[name] = el.checked;
    else if (s.type === "int" || s.type === "float") {
      if (el.value === "") continue;
      out[name] = Number(el.value);
    } else out[name] = el.value;
  }
  return out;
}

function fillForm(form, values) {
  for (const [k, v] of Object.entries(values)) {
    const el = form.elements.namedItem(k);
    if (!el) continue;
    if (el.type === "checkbox") el.checked = !!v; else el.value = v ?? "";
  }
}

function presetBar(presets, form, onPick) {
  const bar = h("div", { class: "presets", role: "group", "aria-label": "Presets" });
  for (const p of presets) {
    bar.append(h("button", {
      type: "button", class: "chip", title: p.help || "",
      onclick: (e) => { fillForm(form, p.values); bar.querySelectorAll(".chip").forEach((c) => c.classList.remove("active")); e.target.classList.add("active"); onPick && onPick(p); },
    }, p.name));
  }
  return bar;
}

const MARKET_PRESETS = [
  { name: "Realistic", values: { rate: 2000, vol: 1.5, informed: 0.6, impact: 0.15 }, help: "Default: informed flow and price impact" },
  { name: "Calm", values: { rate: 800, vol: 0.8, informed: 0.3, impact: 0.05 }, help: "Quiet market, little information in the flow" },
  { name: "Volatile", values: { rate: 4000, vol: 4, informed: 0.6, impact: 0.2 }, help: "Busy, fast-moving market" },
  { name: "Toxic flow", values: { rate: 2000, vol: 1.5, informed: 0.9, impact: 0.25 }, help: "Most takers are informed; tough for market makers" },
  { name: "Noise only", values: { rate: 2000, vol: 1.5, informed: 0, impact: 0 }, help: "Uninformed traders, no impact: market making looks too easy" },
];

function marketFields(schema, values = {}) {
  return [numberField("rate", schema, values.rate), numberField("vol", schema, values.vol),
    numberField("informed", schema, values.informed), numberField("impact", schema, values.impact)];
}

function jobStatusBadge(job) {
  const map = { queued: "warn", running: "run", succeeded: "ok", failed: "err", cancelled: "" };
  return h("span", { class: `badge ${map[job.status] || ""}` }, job.status === "queued" && job.queue_position ? `queued #${job.queue_position}` : job.status);
}

function downloads(job) {
  if (!job.files || !job.files.length) return null;
  return h("div", { class: "btn-row" }, h("span", { class: "muted" }, "Downloads:"),
    job.files.map((f) => h("a", { class: "btn small", href: `/api/jobs/${job.id}/files/${f}`, download: "" }, f)));
}

function progressCard(job, label) {
  const elapsed = job.started_at ? (Date.now() / 1000 - job.started_at) : 0;
  return h("div", { class: "card" },
    h("div", { class: "card-head" }, h("h2", {}, label), jobStatusBadge(job)),
    h("div", { class: "progress" }, h("div")),
    h("p", { class: "muted" }, job.status === "queued"
      ? `Waiting for a free worker${job.queue_position ? ` (position ${job.queue_position})` : ""}…`
      : `Running for ${fmt.dur(elapsed)}… The C++ engine is working; results appear here automatically.`),
    h("div", { class: "btn-row" }, h("button", { class: "btn small danger", onclick: () => cancelJob(job.id) }, "Cancel")));
}

async function cancelJob(id) {
  try { await api(`/api/jobs/${id}`, { method: "DELETE" }); toast("Cancelled"); } catch (e) { reportError(e); }
}

function failedCard(job) {
  return h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Run failed"), jobStatusBadge(job)),
    h("div", { class: "notice err" }, job.error || "Unknown error"),
    downloads(job));
}

function jobList(jobs, selectedId, onSelect, emptyText) {
  if (!jobs.length) return h("div", { class: "muted" }, emptyText);
  return h("div", { class: "joblist" }, jobs.map((j) =>
    h("div", { class: "jobitem" + (j.id === selectedId ? " selected" : ""), dataset: { id: j.id }, role: "button", tabindex: "0", onclick: () => onSelect(j.id), onkeydown: (e) => { if (e.key === "Enter") onSelect(j.id); } },
      h("div", {}, h("div", {}, j.headline || describeParams(j)), h("div", { class: "t" }, fmt.when(j.created_at))),
      jobStatusBadge(j))));
}

function describeParams(j) {
  const p = j.params || {};
  if (j.kind === "simulation") return `${fmt.int(p.events)} events · ${p.queue}`;
  if (j.kind === "backtest") return `${p.strategy === "mm" ? "Market maker" : "Imbalance taker"} · ${p.source === "synthetic" ? "synthetic" : "saved data"}`;
  if (j.kind === "benchmark") return `${fmtNum(p.messages, 0)} msgs × ${p.reps}`;
  return j.kind;
}

function kpi(label, value, { sub, cls, help } = {}) {
  return h("div", { class: "kpi" },
    h("div", { class: "label" }, label, help ? tip(help) : null),
    h("div", { class: "value " + (cls || "") }, value),
    sub ? h("div", { class: "sub" }, sub) : null);
}

function engineBanner() {
  const i = state.info;
  if (!i) return h("div", { class: "notice err" }, "Cannot reach the backend. Is the server running?");
  if (i.engines_missing && i.engines_missing.length)
    return h("div", { class: "notice err" }, `The C++ engines are missing on the server (${i.engines_missing.join(", ")}). Build the project and set MDS_BIN_DIR.`);
  return null;
}

function addChart(chart) { state.cleanup.push(() => chart.destroy()); return chart; }

// =============================================================================
// Page: Live market
// =============================================================================

function pageLive(view) {
  const schema = state.info?.schemas?.live;
  const st = live.status;
  view.append(h("div", { class: "page-head" },
    h("div", {}, h("h1", {}, "Live market"),
      h("p", {}, "A simulated exchange running in real time. The C++ engine paces order flow to the clock, hands every event to a second thread through a lock-free queue, rebuilds the order book and lets a strategy trade against it. Everyone viewing this page sees the same market.")),
    h("div", { class: "btn-row" },
      h("button", { class: "btn", id: "live-settings-btn", onclick: () => $("#live-settings").classList.toggle("hidden") }, "⚙ Market settings"),
      h("button", { class: "btn primary", id: "live-start", onclick: () => startLive() }, "Restart"),
      h("button", { class: "btn", id: "live-stop", onclick: () => stopLive() }, "Stop"))));

  const banner = engineBanner();
  if (banner) view.append(banner);

  // Settings panel
  if (schema) {
    const form = h("form", { id: "live-form", onsubmit: (e) => { e.preventDefault(); startLive(readForm(form, schema), form); } });
    const params = st?.params || {};
    const stratPick = h("div", { class: "choice-cards" });
    const stratInput = h("input", { type: "hidden", name: "strategy", value: params.strategy || "mm" });
    const strategies = [
      ["mm", "Market maker", "Quotes both sides, earns the spread and rebates, risks being picked off."],
      ["obi", "Imbalance taker", "Buys/sells aggressively when the book is lopsided."],
      ["none", "Watch only", "No strategy; just the market."],
    ];
    for (const [k, title, desc] of strategies) {
      const card = h("div", { class: "choice" + ((params.strategy || "mm") === k ? " active" : ""), role: "radio", tabindex: "0",
        onclick: () => { stratInput.value = k; stratPick.querySelectorAll(".choice").forEach((c) => c.classList.remove("active")); card.classList.add("active"); } },
        h("b", {}, title), h("span", {}, desc));
      stratPick.append(card);
    }
    form.append(
      h("div", { class: "field span-2" }, h("label", {}, "Strategy"), stratPick, stratInput),
      h("h3", { class: "muted" }, "Market"),
    );
    const fields = h("div", { class: "fields" }, marketFields(schema, params),
      numberField("speed", schema, params.speed), numberField("latency_us", schema, params.latency_us),
      numberField("snapshot_ms", schema, params.snapshot_ms), numberField("seed", schema, params.seed));
    form.append(presetBar(MARKET_PRESETS, form), fields,
      h("div", { class: "btn-row" }, h("button", { class: "btn primary", type: "submit" }, "Apply & restart market")));
    view.append(h("div", { class: "card hidden", id: "live-settings" }, form));
    view.lastChild.style.marginBottom = "14px";
  }

  const kpis = h("div", { class: "kpis", id: "live-kpis" });
  view.append(kpis);

  const ladder = h("div", { class: "ladder", id: "ladder" });
  const tape = h("div", { class: "tape", id: "tape" });
  const priceCanvas = h("canvas", { class: "chart tall", "aria-label": "Mid price chart" });
  const pnlCanvas = h("canvas", { class: "chart short", "aria-label": "Strategy PnL chart" });
  const stratBox = h("div", { id: "strat" });
  const fillsBox = h("div", { class: "tape", id: "strat-fills" });

  view.append(h("div", { class: "grid live-grid" },
    h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Order book"), h("span", { class: "hint" }, "aggregated by price")), ladder),
    h("div", { class: "stack" },
      h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Price"),
        h("span", { class: "hint" }, "mid · ", h("span", { class: "bid" }, "▲ strategy buys"), " · ", h("span", { class: "ask" }, "▼ strategy sells"))), priceCanvas),
      h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Strategy PnL & position"), h("span", { class: "hint" }, "paper trading")), pnlCanvas)),
    h("div", { class: "stack col-3" },
      h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Strategy"), tip("PnL = realized + unrealized − fees. Negative fees are maker rebates earned.")), stratBox,
        h("h3", { class: "muted" }, "Recent fills"), fillsBox),
      h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Trades"), h("span", { class: "hint" }, "time · price · size")), tape))));

  const priceChart = addChart(new LineChart(priceCanvas, { xFormat: fmt.clock, yFormat: (v) => v.toFixed(2), emptyText: "Waiting for market data…" }));
  const pnlChart = addChart(new LineChart(pnlCanvas, { xFormat: fmt.clock, yFormat: (v) => fmtNum(v, 0), y2Format: (v) => v.toFixed(0), zeroLine: true, emptyText: "Strategy starts after warm-up…" }));

  let lastDraw = 0;
  const render = (kind) => {
    const t = live.tick;
    const s = live.status;
    $("#live-start").textContent = s && s.state === "stopped" ? "Start" : "Restart";
    $("#live-stop").disabled = !s || s.state === "stopped";
    if (s && s.state !== "running" && kind !== "tick") {
      const msg = { stopped: "The live market is stopped. Press Start to run it.", paused: "The live market is paused while a benchmark runs; it resumes automatically.", restarting: "The market engine is restarting…" }[s.state];
      kpis.replaceChildren(...[msg && h("div", { class: "notice warn" }, msg), s.error && h("div", { class: "notice err" }, "Engine error: " + s.error)].filter(Boolean));
      return;
    }
    if (!t) return;
    const b = t.book, st = t.strategy;
    kpis.replaceChildren(
      kpi("Mid price", fmt.px(b.mid), { sub: fmt.clock(t.sim_ts / 1e6) }),
      kpi("Spread", fmt.px(b.spread), { sub: `${fmt.int(b.orders)} resting orders` }),
      kpi("Events/s", fmt.int(t.events_per_sec), { sub: `${fmt.int(t.events)} total`, help: "Order book events processed per second right now." }),
      kpi("Hand-off latency", fmt.us(t.latency_us.p50), { sub: `p99 ${fmt.us(t.latency_us.p99)}`, help: "Time from the feed thread enqueuing an event to the book being updated on the other thread (median; p99 below)." }),
      kpi("Trades", fmt.int(t.trade_count), { sub: `${fmt.int(t.volume)} shares this tick` }),
      kpi("Imbalance", (b.imbalance >= 0 ? "+" : "") + b.imbalance.toFixed(2), { cls: fmt.sign(b.imbalance), help: "(bid size − ask size) / total over the top 5 levels. Positive = more buyers waiting." }),
      kpi("Queue depth", `${t.queue.depth}/${fmt.int(t.queue.capacity)}`, { sub: `${fmt.int(t.queue.producer_stalls)} stalls`, help: "Events waiting in the lock-free ring between the two threads." }),
    );
    renderLadder(ladder, b);
    renderTape(tape, live.trades.slice(0, 22), (r) => [fmt.clock(r[0] / 1e6), r[1].toFixed(2), fmt.int(r[2])], (r) => (r[3] === "B" ? "bid" : "ask"));
    renderTape(fillsBox, live.fills.slice(0, 8), (r) => [`${r[1] === "B" ? "BUY" : "SELL"} ${fmt.int(r[3])}`, r[2].toFixed(2), r[4] ? "maker" : "taker"], (r) => (r[1] === "B" ? "bid" : "ask"));
    stratBox.replaceChildren(
      h("div", { class: "kpis" },
        kpi("PnL", fmt.signedMoney(st.pnl), { cls: fmt.sign(st.pnl) }),
        kpi("Position", fmt.int(st.position), { sub: "shares" })),
      h("table", {}, h("tbody", {},
        row("Strategy", st.trading ? st.name.replace(/_/g, " ") : `${st.name.replace(/_/g, " ")} (warming up)`),
        row("Realized", fmt.money(st.realized)), row("Fees / rebates", fmt.money(-st.fees)),
        row("Fills / volume", `${fmt.int(st.fills_total)} / ${fmt.int(st.volume_total)}`),
        row("Orders sent / working", `${fmt.int(st.orders_sent)} / ${fmt.int(st.working_orders)}`))));
    const now = performance.now();
    if (now - lastDraw > 200 || kind !== "tick") {
      lastDraw = now;
      const pts = live.points;
      const t0 = pts.length ? pts[0][0] : 0;
      const markers = live.fills.filter((f) => f[0] / 1e6 >= t0).map((f) => ({ x: f[0] / 1e6, y: f[2], up: f[1] === "B", color: f[1] === "B" ? "--bid" : "--ask" }));
      priceChart.setData([{ name: "mid", color: "--series-1", points: pts.map((p) => [p[0], p[1]]) }], markers);
      pnlChart.setData([
        { name: "PnL $", color: "--series-1", points: pts.map((p) => [p[0], p[2]]), fill: true },
        { name: "position", color: "--series-3", points: pts.map((p) => [p[0], p[3]]), axis: "right", step: true, width: 1 },
      ]);
    }
  };
  const row = (k, v) => h("tr", {}, h("td", { class: "muted" }, k), h("td", { class: "num" }, v));
  state.cleanup.push(live.on(render));
  render("init");
}

function renderLadder(el, book) {
  const n = 12;
  const asks = (book.asks || []).slice(0, n).reverse();
  const bids = (book.bids || []).slice(0, n);
  const maxQ = Math.max(1, ...asks.map((l) => l[1]), ...bids.map((l) => l[1]));
  const line = (l, side) => {
    const bar = h("div", { class: "bar" });
    bar.style.width = `${(l[1] / maxQ) * 100}%`;
    return h("div", { class: `row ${side}` }, bar, h("span", { class: side }, l[0].toFixed(2)), h("span", {}, fmt.int(l[1])), h("span", { class: "muted" }, l[2]));
  };
  el.replaceChildren(
    h("div", { class: "row head" }, h("div", { class: "bar hidden" }), h("span", {}, "Price"), h("span", {}, "Size"), h("span", {}, "Ord")),
    ...asks.map((l) => line(l, "ask")),
    h("div", { class: "spread" }, book.spread !== undefined ? `spread ${book.spread.toFixed(2)} · mid ${book.mid.toFixed(3)}` : "one-sided book"),
    ...bids.map((l) => line(l, "bid")));
}

function renderTape(el, rows, cells, sideCls) {
  if (!rows.length) { el.replaceChildren(h("div", { class: "muted" }, "Nothing yet")); return; }
  el.replaceChildren(...rows.map((r) => h("div", { class: "row" }, cells(r).map((c, i) => h("span", { class: i === 1 ? sideCls(r) : "" }, c)))));
}

async function startLive(params, form) {
  try {
    await api("/api/live/start", { method: "POST", body: params ? { params } : {} });
    toast("Market (re)started", "ok");
    $("#live-settings")?.classList.add("hidden");
  } catch (e) { reportError(e, form); }
}
async function stopLive() {
  try { await api("/api/live/stop", { method: "POST" }); toast("Market stopped"); } catch (e) { reportError(e); }
}

// =============================================================================
// Page: Simulate (throughput: lock-free vs mutex)
// =============================================================================

async function pageSimulate(view, selected) {
  const schema = state.info?.schemas?.simulation;
  view.append(h("div", { class: "page-head" }, h("div", {}, h("h1", {}, "Simulate"),
    h("p", {}, "Generate a batch of market data and push it through the two-thread pipeline as fast as possible, once with the lock-free queue and once with a mutex queue, to compare throughput and latency. Each run also checks that the rebuilt order book matches the exchange's exactly."))));
  const banner = engineBanner();
  if (banner) view.append(banner);
  if (!schema) return;

  const form = h("form", { onsubmit: (e) => { e.preventDefault(); submit(); } });
  const redisOk = state.info.redis?.configured;
  form.append(
    presetBar([
      { name: "Quick · 200K", values: { events: 200000 } },
      { name: "Standard · 1M", values: { events: 1000000 } },
      { name: `Large · ${fmtNum(schema.events.max, 0)}`, values: { events: schema.events.max } },
    ], form),
    h("div", { class: "fields" },
      numberField("events", schema), numberField("seed", schema),
      selectField("queue", schema, null, { both: "Both (compare)", spsc: "Lock-free only", mutex: "Mutex only" }),
      numberField("batch", schema),
      boolField("save_events", schema), boolField("redis", schema, false, !redisOk, redisOk ? null : "Redis not configured on this server")),
    h("details", { class: "advanced" }, h("summary", {}, "Market & queue settings"),
      presetBar(MARKET_PRESETS, form), h("div", { class: "fields" }, marketFields(schema), numberField("capacity", schema))),
    h("div", { class: "btn-row" }, h("button", { class: "btn primary", type: "submit" }, "Run simulation")));

  const listBox = h("div");
  const detail = h("div", { class: "stack" });
  view.append(h("div", { class: "grid cols-side" },
    h("div", { class: "stack" }, h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "New simulation")), form),
      h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Recent simulations")), listBox)),
    detail));

  let current = selected, stopWatch = null;
  state.cleanup.push(() => stopWatch && stopWatch());
  const refreshList = async () => {
    const { jobs } = await api("/api/jobs?kind=simulation");
    listBox.replaceChildren(jobList(jobs, current, select, "No simulations yet. Run one to see results here."));
    if (!current && jobs.length) select(jobs[0].id);
    if (!jobs.length) detail.replaceChildren(h("div", { class: "card empty-state" }, h("h2", {}, "No results yet"), h("p", {}, "Choose a size on the left and press Run. A 1M-event run takes a few seconds.")));
  };
  const select = (id) => {
    current = id;
    listBox.querySelectorAll(".jobitem").forEach((el) => el.classList.toggle("selected", el.dataset.id === id));
    history.replaceState(null, "", `#/simulate/${id}`);
    stopWatch && stopWatch();
    stopWatch = watchJob(id, (job) => { renderSimulation(detail, job); if (!["queued", "running"].includes(job.status)) refreshList(); });
  };
  const submit = async () => {
    try {
      const job = await api("/api/jobs", { method: "POST", body: { kind: "simulation", params: readForm(form, schema) } });
      toast("Simulation started", "ok");
      await refreshList();
      select(job.id);
    } catch (e) { reportError(e, form); }
  };
  await refreshList();
  if (selected) select(selected);
}

function renderSimulation(el, job) {
  if (["queued", "running"].includes(job.status)) return el.replaceChildren(progressCard(job, "Simulation"));
  if (job.status !== "succeeded") return el.replaceChildren(failedCard(job));
  const r = job.result;
  const pipes = r.pipelines || [];
  const spsc = pipes.find((p) => p.queue.startsWith("spsc"));
  const mutex = pipes.find((p) => p.queue === "mutex");
  const allOk = pipes.every((p) => p.consistent);
  const thru = h("canvas", { class: "chart short" });
  const depth = h("canvas", { class: "chart short" });
  el.replaceChildren(
    h("div", { class: "kpis" },
      spsc ? kpi("Lock-free throughput", fmt.rate(spsc.events_per_sec), { sub: `${fmt.dur(spsc.seconds)} for ${fmt.int(r.generate.events)} events` }) : null,
      mutex ? kpi("Mutex throughput", fmt.rate(mutex.events_per_sec), { sub: `${fmt.dur(mutex.seconds)}` }) : null,
      r.speedup ? kpi("Speed-up", r.speedup.toFixed(2) + "×", { cls: "pos", help: "Lock-free events/s ÷ mutex events/s on identical data and identical book work." }) : null,
      kpi("Book check", allOk ? "exact match" : "MISMATCH", { cls: allOk ? "pos" : "neg", help: "The order book rebuilt on the consumer thread is compared level-by-level with the exchange's own book." }),
      kpi("Simulated time", fmt.dur(r.generate.sim_span_s), { sub: `seed ${r.generate.seed}` })),
    h("div", { class: "grid cols-2" },
      h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Throughput"), h("span", { class: "hint" }, "events per second")), thru),
      h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Final order book depth"), h("span", { class: "hint" }, "cumulative size")), depth)),
    h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Queue latency"),
      tip("Enqueue → book updated, sampled 1 in 64. In this flat-out replay the producer outruns the consumer, so this mostly measures time spent waiting in a full queue, not hand-off cost. The Live page shows true per-message latency.")),
      h("div", { class: "table-wrap" }, h("table", {},
        h("thead", {}, h("tr", {}, ["Queue", "p50", "p90", "p99", "p99.9", "max", "producer stalls"].map((c, i) => h("th", { class: i ? "num" : "" }, c)))),
        h("tbody", {}, pipes.map((p) => h("tr", {}, h("td", {}, p.queue), ...["p50", "p90", "p99", "p999", "max"].map((k) => h("td", { class: "num" }, fmt.ns(p.latency_ns[k]))), h("td", { class: "num" }, fmt.int(p.producer_stalls)))))))),
    h("div", { class: "card" }, h("div", { class: "btn-row" },
      job.files.includes("ticks.bin") ? h("a", { class: "btn primary", href: `#/backtest?source=simulation:${job.id}` }, "Backtest on this data →") : null,
      r.redis_symbol ? h("span", { class: "badge ok" }, `stored in Redis as ${r.redis_symbol}`) : null),
      h("div", { class: "muted" }, " "), downloads(job)));
  addChart(new BarChart(thru)).setData(pipes.map((p) => ({ label: p.queue === "mutex" ? "mutex" : "lock-free", value: p.events_per_sec, color: p.queue === "mutex" ? "--series-3" : "--series-1" })), { format: (v) => fmtNum(v, 2) + "/s" });
  addChart(new DepthChart(depth)).setData(r.book.bids, r.book.asks);
}

// =============================================================================
// Page: Backtest
// =============================================================================

async function pageBacktest(view, selected, query) {
  const schema = state.info?.schemas?.backtest;
  view.append(h("div", { class: "page-head" }, h("div", {}, h("h1", {}, "Backtest"),
    h("p", {}, "Replay market data through a trading strategy. Orders are delayed by your latency, queue position at each price is tracked exactly, and fees and rebates are applied, so the profit you see is explainable rather than optimistic."))));
  const banner = engineBanner();
  if (banner) view.append(banner);
  if (!schema) return;

  const sims = (await api("/api/jobs?kind=simulation")).jobs.filter((j) => j.status === "succeeded" && j.files.includes("ticks.bin"));
  const form = h("form", { onsubmit: (e) => { e.preventDefault(); submit(); } });
  const stratInput = h("input", { type: "hidden", name: "strategy", value: "mm" });
  const stratPick = h("div", { class: "choice-cards" });
  for (const [k, title, desc] of [["mm", "Market maker", "Quotes both sides around the mid, skews quotes against inventory, earns spread + rebates."], ["obi", "Imbalance taker", "Crosses the spread when the top of the book is heavily one-sided."]]) {
    const c = h("div", { class: "choice" + (k === "mm" ? " active" : ""), role: "radio", tabindex: "0",
      onclick: () => { stratInput.value = k; stratPick.querySelectorAll(".choice").forEach((x) => x.classList.remove("active")); c.classList.add("active"); } }, h("b", {}, title), h("span", {}, desc));
    stratPick.append(c);
  }
  const source = h("select", { name: "source", id: "f-source" },
    h("option", { value: "synthetic" }, "Fresh synthetic market"),
    sims.map((j) => h("option", { value: `simulation:${j.id}`, selected: query.get("source") === `simulation:${j.id}` }, `Saved simulation · ${fmt.int(j.params.events)} events · ${fmt.when(j.created_at)}`)));
  const synthOnly = h("div", { class: "fields" }, numberField("events", schema), numberField("seed", schema));
  const marketBox = h("div", {}, presetBar(MARKET_PRESETS, form), h("div", { class: "fields" }, marketFields(schema)));
  const syncSource = () => { const syn = source.value === "synthetic"; synthOnly.classList.toggle("hidden", !syn); marketBox.classList.toggle("hidden", !syn); };
  source.addEventListener("change", syncSource);
  form.append(
    h("div", { class: "field" }, h("label", {}, "Strategy"), stratPick, stratInput),
    h("div", { class: "field" }, h("label", { for: "f-source" }, "Market data", tip("Use a fresh synthetic market, or replay exactly the events saved by a previous simulation.")), source),
    synthOnly,
    presetBar([
      { name: "Realistic", values: { latency_us: 50, maker_bps: -0.2, taker_bps: 0.3 } },
      { name: "Slow link (500 µs)", values: { latency_us: 500 } },
      { name: "Co-located (5 µs)", values: { latency_us: 5 } },
      { name: "No fees", values: { maker_bps: 0, taker_bps: 0 } },
    ], form),
    h("div", { class: "fields" }, numberField("latency_us", schema), numberField("maker_bps", schema), numberField("taker_bps", schema)),
    h("details", { class: "advanced" }, h("summary", {}, "Market behaviour (synthetic data only)"), marketBox),
    h("div", { class: "btn-row" }, h("button", { class: "btn primary", type: "submit" }, "Run backtest")));
  syncSource();

  const listBox = h("div");
  const detail = h("div", { class: "stack" });
  view.append(h("div", { class: "grid cols-side" },
    h("div", { class: "stack" }, h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "New backtest")), form),
      h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Recent backtests")), listBox)),
    detail));

  let current = selected, stopWatch = null;
  state.cleanup.push(() => stopWatch && stopWatch());
  const refreshList = async () => {
    const { jobs } = await api("/api/jobs?kind=backtest");
    listBox.replaceChildren(jobList(jobs, current, select, "No backtests yet."));
    if (!current && jobs.length) select(jobs[0].id);
    if (!jobs.length) detail.replaceChildren(h("div", { class: "card empty-state" }, h("h2", {}, "No results yet"), h("p", {}, "Pick a strategy and press Run. 1M events (about 8 simulated minutes) take under a second.")));
  };
  const select = (id) => {
    current = id;
    listBox.querySelectorAll(".jobitem").forEach((el) => el.classList.toggle("selected", el.dataset.id === id));
    history.replaceState(null, "", `#/backtest/${id}`);
    stopWatch && stopWatch();
    stopWatch = watchJob(id, (job) => { renderBacktest(detail, job); if (!["queued", "running"].includes(job.status)) refreshList(); });
  };
  const submit = async () => {
    const params = readForm(form, schema);
    params.strategy = stratInput.value;
    params.source = source.value;
    if (params.source !== "synthetic") for (const k of ["events", "seed", "rate", "vol", "informed", "impact"]) delete params[k];
    try {
      const job = await api("/api/jobs", { method: "POST", body: { kind: "backtest", params } });
      toast("Backtest started", "ok");
      await refreshList();
      select(job.id);
    } catch (e) { reportError(e, form); }
  };
  await refreshList();
  if (selected) select(selected);
}

function explainBacktest(r) {
  const m = r.metrics, fs = r.fill_stats;
  const trading = m.realized_pnl + m.unrealized_pnl;
  const parts = [];
  parts.push(`Over ${fmt.dur((r.sim_end_ns - r.sim_start_ns) / 1e9)} of simulated trading the ${r.strategy.replace(/_/g, " ")} made ${fmt.int(m.fills)} fills (${fmt.int(m.volume)} shares).`);
  parts.push(`Its trades ${trading >= 0 ? "made" : "lost"} ${fmt.money(Math.abs(trading))}, and it ${m.fees < 0 ? "earned" : "paid"} ${fmt.money(Math.abs(m.fees))} in ${m.fees < 0 ? "maker rebates" : "fees"}, for a total of ${fmt.signedMoney(m.total_pnl)}.`);
  if (fs.count) parts.push(fs.markout_1s < 0 ? `One second after its fills the price had typically moved against it (${fmt.perShare(fs.markout_1s)}/share): it was being picked off by better-informed traders.` : `One second after its fills the price had typically moved in its favour (${fmt.perShare(fs.markout_1s)}/share).`);
  return parts.join(" ");
}

function renderBacktest(el, job) {
  if (["queued", "running"].includes(job.status)) return el.replaceChildren(progressCard(job, "Backtest"));
  if (job.status !== "succeeded") return el.replaceChildren(failedCard(job));
  const r = job.result, m = r.metrics, fs = r.fill_stats;
  const eqCanvas = h("canvas", { class: "chart" });
  const posCanvas = h("canvas", { class: "chart short" });
  const fillsRows = (r.fills_recent || []).slice().reverse().slice(0, 100);
  el.replaceChildren(
    h("div", { class: "card" }, h("p", {}, explainBacktest(r)),
      h("div", { class: "btn-row" }, h("span", { class: "badge" }, r.source === "synthetic" ? "synthetic data" : "saved simulation data"),
        h("span", { class: "badge" }, `latency ${job.params.latency_us} µs`), h("span", { class: "badge" }, `${fmt.int(r.events_per_sec)} events/s`))),
    h("div", { class: "kpis" },
      kpi("Total PnL", fmt.signedMoney(m.total_pnl), { cls: fmt.sign(m.total_pnl) }),
      kpi("Trading PnL", fmt.signedMoney(m.realized_pnl + m.unrealized_pnl), { cls: fmt.sign(m.realized_pnl + m.unrealized_pnl), help: "Profit from the trades themselves, before fees and rebates." }),
      kpi("Fees / rebates", fmt.signedMoney(-m.fees), { cls: fmt.sign(-m.fees), help: "Positive = net rebates earned as a maker." }),
      kpi("Max drawdown", fmt.money(m.max_drawdown), { sub: `over ${fmt.dur(m.max_drawdown_duration_s)}` }),
      kpi("Win rate", fmt.pct(m.win_rate), { help: "Share of 1-second intervals in which equity went up." }),
      kpi("Fills", fmt.int(m.fills), { sub: `${fmt.int(m.volume)} shares · max pos ${fmt.int(m.max_abs_position)}` }),
      kpi("Maker share", fmt.pct(fs.maker_ratio)),
      kpi("1s mark-out", fmt.perShare(fs.markout_1s), { cls: fmt.sign(fs.markout_1s), sub: "per share", help: "Average price move 1 second after each fill, in your favour (+) or against you (−). Negative means adverse selection." }),
      kpi("Sharpe", r.sharpe_reliable ? m.sharpe_annualized.toFixed(1) : "n/a", { sub: r.sharpe_reliable ? "annualised" : `only ${m.equity_samples} samples`, help: "Annualised from 1-second PnL changes. Hidden when there are fewer than 250 samples, where it is mostly noise." })),
    h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Equity"), h("span", { class: "hint" }, "PnL (left) and mid price (right)")), eqCanvas),
    h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Position")), posCanvas),
    h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Fills"), h("span", { class: "hint" }, `latest ${fillsRows.length} of ${fmt.int(fs.count)}`)),
      fillsRows.length ? h("div", { class: "table-wrap scroll-y" }, h("table", {},
        h("thead", {}, h("tr", {}, ["Time (s)", "Side", "Price", "Qty", "Type", "Fee"].map((c, i) => h("th", { class: i && i !== 1 && i !== 4 ? "num" : "" }, c)))),
        h("tbody", {}, fillsRows.map((f) => h("tr", {}, h("td", { class: "num" }, f[0].toFixed(3)), h("td", { class: f[1] === "BUY" ? "bid" : "ask" }, f[1]),
          h("td", { class: "num" }, f[2].toFixed(2)), h("td", { class: "num" }, fmt.int(f[3])), h("td", {}, f[4] ? "maker" : "taker"), h("td", { class: "num" }, fmt.signedMoney(-f[5]))))))) : h("div", { class: "muted" }, "No fills.")),
    h("div", { class: "card" }, downloads(job)));
  const eq = r.equity || [];
  addChart(new LineChart(eqCanvas, { xFormat: (v) => fmt.dur(v), yFormat: (v) => fmtNum(v, 0), y2Format: (v) => v.toFixed(2), zeroLine: true }))
    .setData([{ name: "PnL $", color: "--series-1", points: eq.map((p) => [p[0], p[1]]), fill: true },
      { name: "mid", color: "--series-2", points: eq.map((p) => [p[0], p[2]]), axis: "right", width: 1 }]);
  addChart(new LineChart(posCanvas, { xFormat: (v) => fmt.dur(v), yFormat: (v) => v.toFixed(0), zeroLine: true }))
    .setData([{ name: "position", color: "--series-3", points: eq.map((p) => [p[0], p[3]]), step: true }]);
}

// =============================================================================
// Page: Benchmarks
// =============================================================================

async function pageBenchmarks(view, selected) {
  const schema = state.info?.schemas?.benchmark;
  view.append(h("div", { class: "page-head" }, h("div", {}, h("h1", {}, "Benchmarks"),
    h("p", {}, "Measure on this server how much faster the lock-free queue is than a mutex-based one, and how fast the order book is. The live market pauses while a benchmark runs so the numbers are clean."))));
  const banner = engineBanner();
  if (banner) view.append(banner);
  if (!schema) return;
  const form = h("form", { onsubmit: (e) => { e.preventDefault(); submit(); } });
  form.append(
    presetBar([
      { name: "Quick (~15 s)", values: { messages: 2000000, events: 200000, reps: 3 } },
      { name: "Standard (~1 min)", values: { messages: 10000000, events: 1000000, reps: 5 } },
    ], form),
    h("div", { class: "fields" }, numberField("messages", schema), numberField("events", schema), numberField("reps", schema)),
    h("div", { class: "notice" }, "Results depend on the machine: shared cloud CPUs are noisier than a dedicated one. Each configuration is repeated and the median is shown."),
    h("div", { class: "btn-row" }, h("button", { class: "btn primary", type: "submit" }, "Run benchmark")));
  form.querySelector(".notice").style.margin = "10px 0";
  const listBox = h("div");
  const detail = h("div", { class: "stack" });
  view.append(h("div", { class: "grid cols-side" },
    h("div", { class: "stack" }, h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "New benchmark")), form),
      h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Previous runs")), listBox)),
    detail));
  let current = selected, stopWatch = null;
  state.cleanup.push(() => stopWatch && stopWatch());
  const refreshList = async () => {
    const { jobs } = await api("/api/jobs?kind=benchmark");
    listBox.replaceChildren(jobList(jobs, current, select, "No benchmark runs yet."));
    if (!current && jobs.length) select(jobs[0].id);
    if (!jobs.length) detail.replaceChildren(h("div", { class: "card empty-state" }, h("h2", {}, "No benchmark yet"), h("p", {}, "Start with Quick to get numbers in about 15 seconds.")));
  };
  const select = (id) => {
    current = id;
    listBox.querySelectorAll(".jobitem").forEach((el) => el.classList.toggle("selected", el.dataset.id === id));
    history.replaceState(null, "", `#/benchmarks/${id}`);
    stopWatch && stopWatch();
    stopWatch = watchJob(id, (job) => { renderBenchmark(detail, job); if (!["queued", "running"].includes(job.status)) refreshList(); });
  };
  const submit = async () => {
    try {
      const job = await api("/api/jobs", { method: "POST", body: { kind: "benchmark", params: readForm(form, schema) } });
      toast("Benchmark started; the live market pauses until it finishes", "ok");
      await refreshList();
      select(job.id);
    } catch (e) { reportError(e, form); }
  };
  await refreshList();
  if (selected) select(selected);
}

function renderBenchmark(el, job) {
  if (["queued", "running"].includes(job.status)) return el.replaceChildren(progressCard(job, "Benchmark"));
  if (job.status !== "succeeded") return el.replaceChildren(failedCard(job));
  const q = job.result.queue, ob = job.result.orderbook;
  const raw = q.rows.filter((r) => r.layer === "raw");
  const pipe = q.rows.filter((r) => r.layer === "pipeline");
  const find = (rows, v) => rows.find((r) => r.variant === v) || {};
  const c1 = h("canvas", { class: "chart short" }), c2 = h("canvas", { class: "chart short" });
  el.replaceChildren(
    h("div", { class: "kpis" },
      kpi("Raw speed-up", (find(raw, "spsc pop 1").vs_mutex || 0).toFixed(1) + "×", { cls: "pos", help: "Lock-free vs mutex queue, one message at a time: the queue cost in isolation." }),
      kpi("Pipeline speed-up", (find(pipe, "spsc batch 1").vs_mutex || 0).toFixed(2) + "×", { cls: "pos", help: "End to end, including rebuilding the order book on the consumer thread." }),
      kpi("Batched pipeline", (find(pipe, "spsc batch 64").vs_mutex || 0).toFixed(2) + "×", { help: "With 64-event batches the mutex is taken once per batch, so the gap narrows." }),
      kpi("Order book", fmt.rate(ob.events_per_sec), { sub: "single thread" }),
      kpi("Machine", `${q.hw_threads} threads`, { sub: `median of ${q.reps}` })),
    h("div", { class: "grid cols-2" },
      h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Raw queue"), h("span", { class: "hint" }, "messages / second")), c1),
      h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Feed → queue → order book"), h("span", { class: "hint" }, "events / second")), c2)),
    h("div", { class: "card" }, h("div", { class: "card-head" }, h("h2", {}, "Order book operation latency"), tip("Includes ~20-40 ns of timer overhead per measurement.")),
      h("div", { class: "table-wrap" }, h("table", {}, h("thead", {}, h("tr", {}, ["Operation", "Samples", "Mean", "p50", "p99", "p99.9"].map((c, i) => h("th", { class: i ? "num" : "" }, c)))),
        h("tbody", {}, ob.ops.map((o) => h("tr", {}, h("td", {}, o.op), h("td", { class: "num" }, fmt.int(o.samples)), ...["mean_ns", "p50_ns", "p99_ns", "p999_ns"].map((k) => h("td", { class: "num" }, fmt.ns(o[k]))))))))),
    h("div", { class: "card" }, downloads(job)));
  const bars = (rows) => rows.map((r) => ({ label: r.variant, value: r.rate, color: r.variant.startsWith("spsc") ? "--series-1" : "--series-3", note: r.variant.startsWith("spsc") ? `${r.vs_mutex.toFixed(2)}×` : "" }));
  addChart(new BarChart(c1)).setData(bars(raw), { format: (v) => fmtNum(v, 1) + "/s" });
  addChart(new BarChart(c2)).setData(bars(pipe), { format: (v) => fmtNum(v, 1) + "/s" });
}

// =============================================================================
// Page: History
// =============================================================================

async function pageHistory(view) {
  view.append(h("div", { class: "page-head" }, h("div", {}, h("h1", {}, "History"), h("p", {}, "Every simulation, backtest and benchmark run on this server. Results and output files are kept until they are deleted or the oldest are pruned."))));
  const filterBar = h("div", { class: "presets" });
  const tableBox = h("div", { class: "card" });
  view.append(filterBar, tableBox);
  let kind = "";
  for (const [k, label] of [["", "All"], ["simulation", "Simulations"], ["backtest", "Backtests"], ["benchmark", "Benchmarks"]]) {
    filterBar.append(h("button", { type: "button", class: "chip" + (k === kind ? " active" : ""), onclick: (e) => { kind = k; filterBar.querySelectorAll(".chip").forEach((c) => c.classList.remove("active")); e.target.classList.add("active"); load(); } }, label));
  }
  let timer = null;
  state.cleanup.push(() => clearTimeout(timer));
  const route = { simulation: "simulate", backtest: "backtest", benchmark: "benchmarks" };
  const load = async () => {
    clearTimeout(timer);
    const { jobs } = await api("/api/jobs" + (kind ? `?kind=${kind}` : ""));
    if (!jobs.length) { tableBox.replaceChildren(h("div", { class: "empty-state" }, h("h2", {}, "Nothing here yet"), h("p", {}, "Runs you start appear here."))); return; }
    tableBox.replaceChildren(h("div", { class: "table-wrap" }, h("table", {},
      h("thead", {}, h("tr", {}, ["Started", "Type", "Summary", "Status", "Duration", ""].map((c) => h("th", {}, c)))),
      h("tbody", {}, jobs.map((j) => h("tr", { class: "clickable", onclick: () => { location.hash = `#/${route[j.kind]}/${j.id}`; } },
        h("td", {}, fmt.when(j.created_at)), h("td", {}, j.kind), h("td", {}, j.headline || describeParams(j)), h("td", {}, jobStatusBadge(j)),
        h("td", {}, j.finished_at && j.started_at ? fmt.dur(j.finished_at - j.started_at) : "–"),
        h("td", {}, h("button", { class: "btn small danger", onclick: async (e) => {
          e.stopPropagation();
          if (!confirm(["queued", "running"].includes(j.status) ? "Cancel this run?" : "Delete this run and its files?")) return;
          try { await api(`/api/jobs/${j.id}`, { method: "DELETE" }); load(); } catch (err) { reportError(err); }
        } }, ["queued", "running"].includes(j.status) ? "Cancel" : "Delete"))))))));
    if (jobs.some((j) => ["queued", "running"].includes(j.status))) timer = setTimeout(load, 2000);
  };
  await load();
}

// =============================================================================
// Page: About
// =============================================================================

function pageAbout(view) {
  const i = state.info || {};
  const box = (title, text) => h("div", { class: "box" }, h("b", {}, title), text);
  const arrow = () => h("div", { class: "arrow" }, "→");
  view.append(
    h("div", { class: "page-head" }, h("div", {}, h("h1", {}, "About"), h("p", {}, "What this application is and how it works."))),
    h("div", { class: "grid cols-2" },
      h("div", { class: "card" }, h("h2", {}, "What is this?"),
        h("p", {}, "Stock exchanges match buyers and sellers in an order book and broadcast every change as market data. Trading firms rebuild that book in their own systems, store the history, and test strategies against it before risking money."),
        h("p", {}, "This app is a complete, small version of that chain, built for learning and demonstrating low-latency C++ engineering. It does not connect to a real exchange and never trades real money."),
        h("h2", {}, "Pages"),
        h("p", {}, h("b", {}, "Live market"), ": a shared, real-time simulated exchange with a strategy paper-trading against it."),
        h("p", {}, h("b", {}, "Simulate"), ": batch runs comparing the lock-free and mutex pipelines."),
        h("p", {}, h("b", {}, "Backtest"), ": replay data through a strategy with latency, queue position and fees."),
        h("p", {}, h("b", {}, "Benchmarks"), ": measure the queue and order book on this server."),
        h("p", {}, h("b", {}, "History"), ": every run, with downloadable outputs.")),
      h("div", { class: "card" }, h("h2", {}, "How it works"),
        h("div", { class: "flow" }, box("Exchange feed", "C++ thread generating realistic order flow"), arrow(), box("Lock-free queue", "wait-free SPSC ring, no locks"), arrow(), box("Order book", "price-time priority, second thread")),
        h("div", { class: "flow" }, box("Strategy", "paper trading with latency + queue position"), arrow(), box("Python backend", "jobs, live stream (SSE), API"), arrow(), box("This browser", "live dashboard & results")),
        h("p", {}, "The heavy lifting is done by compiled C++ engines; the web server only starts them, collects their JSON output and streams it to browsers."),
        h("h2", {}, "This server"),
        h("table", {}, h("tbody", {},
          h("tr", {}, h("td", { class: "muted" }, "Version"), h("td", {}, i.version || "–")),
          h("tr", {}, h("td", { class: "muted" }, "Uptime"), h("td", {}, fmt.dur(i.uptime_s))),
          h("tr", {}, h("td", { class: "muted" }, "Engines"), h("td", {}, i.engines_missing && i.engines_missing.length ? `missing: ${i.engines_missing.join(", ")}` : "all present")),
          h("tr", {}, h("td", { class: "muted" }, "Redis"), h("td", {}, !i.redis?.configured ? "not configured" : i.redis.ok ? "connected" : `unreachable: ${i.redis.error || ""}`)),
          h("tr", {}, h("td", { class: "muted" }, "Max events per run"), h("td", {}, fmt.int(i.limits?.max_events))),
          h("tr", {}, h("td", { class: "muted" }, "Access"), h("td", {}, i.auth_required ? "token required for changes" : "open")))),
        h("p", {}, h("a", { href: "https://github.com/vnsiddhanth/market-data-simulator", target: "_blank", rel: "noopener" }, "Source code, docs and benchmarks on GitHub →")))));
}

// =============================================================================
// Router & bootstrap
// =============================================================================

const ROUTES = { live: pageLive, simulate: pageSimulate, backtest: pageBacktest, benchmarks: pageBenchmarks, history: pageHistory, about: pageAbout };

async function route() {
  const raw = location.hash.replace(/^#\/?/, "") || "live";
  const [pathPart, queryPart] = raw.split("?");
  const [name, arg] = pathPart.split("/");
  const page = ROUTES[name] ? name : "live";
  for (const fn of state.cleanup.splice(0)) { try { fn(); } catch { /* ignore */ } }
  document.querySelectorAll(".nav a").forEach((a) => a.classList.toggle("active", a.dataset.route === page));
  const view = $("#view");
  view.replaceChildren();
  document.title = `${page[0].toUpperCase() + page.slice(1)} · Market Data Simulator`;
  if (!state.info) await loadInfo();
  try {
    await ROUTES[page](view, arg, new URLSearchParams(queryPart || ""));
  } catch (e) {
    view.append(h("div", { class: "notice err" }, `Something went wrong loading this page: ${e.message}`));
  }
}

function openTokenDialog() {
  const d = $("#token-dialog");
  if (d.open) return;
  $("#token-input").value = getToken();
  d.showModal();
}

function initChrome() {
  const saved = (() => { try { return localStorage.getItem("mds-theme"); } catch { return null; } })();
  if (saved) document.documentElement.dataset.theme = saved;
  $("#theme-btn").addEventListener("click", () => {
    const cur = document.documentElement.dataset.theme || (matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light");
    const next = cur === "dark" ? "light" : "dark";
    document.documentElement.dataset.theme = next;
    try { localStorage.setItem("mds-theme", next); } catch { /* ignore */ }
    window.dispatchEvent(new Event("resize"));
    route();
  });
  $("#token-btn").addEventListener("click", openTokenDialog);
  $("#token-dialog").addEventListener("close", () => {
    const v = $("#token-dialog").returnValue;
    if (v === "save") { setToken($("#token-input").value.trim()); toast("Token saved", "ok"); }
    if (v === "clear") { setToken(""); toast("Token cleared"); }
  });
}

initChrome();
loadInfo().then((info) => {
  if (info && info.auth_required && !getToken()) $("#token-btn").classList.add("primary");
  live.connect();
  updateConnPill();
  route();
});
window.addEventListener("hashchange", route);
