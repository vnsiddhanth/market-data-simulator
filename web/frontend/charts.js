// Tiny dependency-free canvas charts: line (multi-series, hover crosshair),
// horizontal bars, and order-book depth. Colours come from CSS variables so
// light/dark themes just work. HiDPI-aware and resize-aware.

const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

function fmtNum(v, digits = 2) {
  if (v === null || v === undefined || !isFinite(v)) return "–";
  const a = Math.abs(v);
  if (a >= 1e9) return (v / 1e9).toFixed(digits) + "B";
  if (a >= 1e6) return (v / 1e6).toFixed(digits) + "M";
  if (a >= 1e4) return (v / 1e3).toFixed(1) + "K";
  return v.toFixed(digits);
}

function niceTicks(min, max, count = 5) {
  if (!isFinite(min) || !isFinite(max)) return [];
  if (min === max) { min -= 1; max += 1; }
  const span = max - min;
  const step0 = span / count;
  const mag = Math.pow(10, Math.floor(Math.log10(step0)));
  const norm = step0 / mag;
  const step = (norm >= 5 ? 10 : norm >= 2 ? 5 : norm >= 1 ? 2 : 1) * mag;
  const ticks = [];
  for (let t = Math.ceil(min / step) * step; t <= max + step * 1e-9; t += step) ticks.push(+t.toPrecision(12));
  return ticks;
}

class BaseChart {
  constructor(canvas, opts = {}) {
    this.canvas = canvas;
    this.ctx = canvas.getContext("2d");
    this.opts = opts;
    this.hoverX = null;
    this.ro = new ResizeObserver(() => this.draw());
    this.ro.observe(canvas);
    canvas.addEventListener("mousemove", (e) => {
      const r = canvas.getBoundingClientRect();
      this.hoverX = e.clientX - r.left;
      this.draw();
    });
    canvas.addEventListener("mouseleave", () => { this.hoverX = null; this.draw(); });
  }
  destroy() { this.ro.disconnect(); }
  setup() {
    const dpr = window.devicePixelRatio || 1;
    const w = this.canvas.clientWidth, h = this.canvas.clientHeight;
    if (!w || !h) return null;
    if (this.canvas.width !== Math.round(w * dpr) || this.canvas.height !== Math.round(h * dpr)) {
      this.canvas.width = Math.round(w * dpr);
      this.canvas.height = Math.round(h * dpr);
    }
    const ctx = this.ctx;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    ctx.font = `11px ${css("--font-mono") || "monospace"}`;
    return { w, h, ctx };
  }
  empty(ctx, w, h, text) {
    ctx.fillStyle = css("--muted");
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(text || "No data yet", w / 2, h / 2);
  }
}

// series: [{name, color (css var), points: [[x,y],...], width, axis: "left"|"right", step: bool, dots: bool}]
export class LineChart extends BaseChart {
  constructor(canvas, opts = {}) {
    super(canvas, opts);
    this.series = [];
    this.markers = []; // [{x, y, color, up: bool}]
  }
  setData(series, markers = []) {
    this.series = series;
    this.markers = markers;
    this.draw();
  }
  draw() {
    const s = this.setup();
    if (!s) return;
    const { w, h, ctx } = s;
    const o = this.opts;
    const pad = { l: 56, r: this.series.some((x) => x.axis === "right") ? 56 : 12, t: 10, b: 22 };
    const pts = this.series.flatMap((x) => x.points);
    if (pts.length < 2) return this.empty(ctx, w, h, o.emptyText);

    let xmin = Infinity, xmax = -Infinity;
    for (const [x] of pts) { if (x < xmin) xmin = x; if (x > xmax) xmax = x; }
    if (xmin === xmax) xmax = xmin + 1;
    const range = (axis) => {
      let lo = Infinity, hi = -Infinity;
      for (const ser of this.series) {
        if ((ser.axis || "left") !== axis) continue;
        for (const [, y] of ser.points) { if (y < lo) lo = y; if (y > hi) hi = y; }
      }
      for (const m of this.markers) if (axis === "left") { if (m.y < lo) lo = m.y; if (m.y > hi) hi = m.y; }
      if (o.zeroLine && axis === "left") { lo = Math.min(lo, 0); hi = Math.max(hi, 0); }
      if (lo === hi) { lo -= 1; hi += 1; }
      const m = (hi - lo) * 0.08;
      return [lo - m, hi + m];
    };
    const [ly0, ly1] = range("left");
    const [ry0, ry1] = this.series.some((x) => x.axis === "right") ? range("right") : [0, 1];
    const X = (x) => pad.l + ((x - xmin) / (xmax - xmin)) * (w - pad.l - pad.r);
    const Y = (y, axis) => {
      const [a, b] = axis === "right" ? [ry0, ry1] : [ly0, ly1];
      return pad.t + (1 - (y - a) / (b - a)) * (h - pad.t - pad.b);
    };

    // grid + axes
    ctx.strokeStyle = css("--grid");
    ctx.fillStyle = css("--muted");
    ctx.lineWidth = 1;
    ctx.textBaseline = "middle";
    for (const t of niceTicks(ly0, ly1, Math.max(2, Math.floor(h / 45)))) {
      const y = Math.round(Y(t, "left")) + 0.5;
      ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(w - pad.r, y); ctx.stroke();
      ctx.textAlign = "right";
      ctx.fillText((o.yFormat || fmtNum)(t), pad.l - 6, y);
    }
    if (pad.r > 12) {
      for (const t of niceTicks(ry0, ry1, Math.max(2, Math.floor(h / 45)))) {
        ctx.textAlign = "left";
        ctx.fillText((o.y2Format || fmtNum)(t), w - pad.r + 6, Y(t, "right"));
      }
    }
    ctx.textAlign = "center";
    ctx.textBaseline = "top";
    for (const t of niceTicks(xmin, xmax, Math.max(2, Math.floor(w / 110)))) {
      ctx.fillText((o.xFormat || fmtNum)(t), X(t), h - pad.b + 6);
    }
    if (o.zeroLine && ly0 < 0 && ly1 > 0) {
      ctx.strokeStyle = css("--muted");
      ctx.setLineDash([3, 3]);
      const y = Math.round(Y(0, "left")) + 0.5;
      ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(w - pad.r, y); ctx.stroke();
      ctx.setLineDash([]);
    }

    // series
    for (const ser of this.series) {
      if (ser.points.length < 1) continue;
      ctx.strokeStyle = css(ser.color) || ser.color;
      ctx.lineWidth = ser.width || 1.5;
      ctx.beginPath();
      ser.points.forEach(([x, y], i) => {
        const px = X(x), py = Y(y, ser.axis || "left");
        if (i === 0) ctx.moveTo(px, py);
        else if (ser.step) { ctx.lineTo(px, Y(ser.points[i - 1][1], ser.axis || "left")); ctx.lineTo(px, py); }
        else ctx.lineTo(px, py);
      });
      ctx.stroke();
      if (ser.fill) {
        ctx.lineTo(X(ser.points[ser.points.length - 1][0]), h - pad.b);
        ctx.lineTo(X(ser.points[0][0]), h - pad.b);
        ctx.closePath();
        ctx.globalAlpha = 0.08;
        ctx.fillStyle = ctx.strokeStyle;
        ctx.fill();
        ctx.globalAlpha = 1;
      }
    }
    for (const m of this.markers) {
      const px = X(m.x), py = Y(m.y, "left");
      ctx.fillStyle = css(m.color) || m.color;
      ctx.beginPath();
      const r = m.r || 3.5;
      if (m.up) { ctx.moveTo(px, py - r); ctx.lineTo(px + r, py + r); ctx.lineTo(px - r, py + r); }
      else { ctx.moveTo(px, py + r); ctx.lineTo(px + r, py - r); ctx.lineTo(px - r, py - r); }
      ctx.closePath();
      ctx.fill();
    }

    // hover crosshair + tooltip
    if (this.hoverX !== null && this.hoverX >= pad.l && this.hoverX <= w - pad.r) {
      const xv = xmin + ((this.hoverX - pad.l) / (w - pad.l - pad.r)) * (xmax - xmin);
      ctx.strokeStyle = css("--muted");
      ctx.beginPath(); ctx.moveTo(this.hoverX + 0.5, pad.t); ctx.lineTo(this.hoverX + 0.5, h - pad.b); ctx.stroke();
      const lines = [(o.xFormat || fmtNum)(xv)];
      for (const ser of this.series) {
        let best = null, bd = Infinity;
        for (const p of ser.points) { const d = Math.abs(p[0] - xv); if (d < bd) { bd = d; best = p; } }
        if (best) {
          lines.push(`${ser.name}: ${(ser.axis === "right" ? (o.y2Format || fmtNum) : (o.yFormat || fmtNum))(best[1])}`);
          ctx.fillStyle = css(ser.color) || ser.color;
          ctx.beginPath(); ctx.arc(X(best[0]), Y(best[1], ser.axis || "left"), 3, 0, 7); ctx.fill();
        }
      }
      const tw = Math.max(...lines.map((l) => ctx.measureText(l).width)) + 14;
      const th = lines.length * 15 + 8;
      let tx = this.hoverX + 10;
      if (tx + tw > w) tx = this.hoverX - tw - 10;
      ctx.fillStyle = css("--tooltip-bg");
      ctx.strokeStyle = css("--border");
      ctx.fillRect(tx, pad.t + 4, tw, th);
      ctx.strokeRect(tx + 0.5, pad.t + 4.5, tw, th);
      ctx.fillStyle = css("--text");
      ctx.textAlign = "left";
      ctx.textBaseline = "top";
      lines.forEach((l, i) => ctx.fillText(l, tx + 7, pad.t + 9 + i * 15));
    }
  }
}

// bars: [{label, value, color, note}]
export class BarChart extends BaseChart {
  setData(bars, opts = {}) {
    this.bars = bars;
    Object.assign(this.opts, opts);
    this.draw();
  }
  draw() {
    const s = this.setup();
    if (!s) return;
    const { w, h, ctx } = s;
    const bars = this.bars || [];
    if (!bars.length) return this.empty(ctx, w, h);
    const fmtV = this.opts.format || fmtNum;
    const valueText = (b) => fmtV(b.value) + (b.note ? `  ${b.note}` : "");
    // Size the label and value columns from the text actually drawn.
    const labelW = Math.min(w * 0.45, Math.max(...bars.map((b) => ctx.measureText(b.label).width)) + 14);
    const valueW = Math.max(...bars.map((b) => ctx.measureText(valueText(b)).width)) + 12;
    const max = Math.max(...bars.map((b) => b.value)) || 1;
    const rowH = Math.min(34, (h - 8) / bars.length);
    bars.forEach((b, i) => {
      const y = 4 + i * rowH;
      const bw = (Math.max(20, w - labelW - valueW) * b.value) / max;
      ctx.fillStyle = css("--text");
      ctx.textAlign = "right";
      ctx.textBaseline = "middle";
      ctx.fillText(b.label, labelW - 8, y + rowH / 2);
      ctx.fillStyle = css(b.color) || b.color;
      ctx.fillRect(labelW, y + rowH * 0.18, Math.max(2, bw), rowH * 0.64);
      ctx.fillStyle = css("--muted");
      ctx.textAlign = "left";
      ctx.fillText(valueText(b), labelW + bw + 6, y + rowH / 2);
    });
  }
}

// bids/asks: [[price, qty, orders], ...] best first
export class DepthChart extends BaseChart {
  setData(bids, asks) {
    this.bids = bids || [];
    this.asks = asks || [];
    this.draw();
  }
  draw() {
    const s = this.setup();
    if (!s) return;
    const { w, h, ctx } = s;
    if (!this.bids.length || !this.asks.length) return this.empty(ctx, w, h);
    const cum = (lv) => { let c = 0; return lv.map(([p, q]) => [p, (c += q)]); };
    const b = cum(this.bids), a = cum(this.asks);
    const pmin = b[b.length - 1][0], pmax = a[a.length - 1][0];
    const qmax = Math.max(b[b.length - 1][1], a[a.length - 1][1]);
    const pad = { l: 8, r: 8, t: 8, b: 20 };
    const X = (p) => pad.l + ((p - pmin) / (pmax - pmin || 1)) * (w - pad.l - pad.r);
    const Y = (q) => pad.t + (1 - q / qmax) * (h - pad.t - pad.b);
    const area = (pts, color) => {
      ctx.beginPath();
      ctx.moveTo(X(pts[0][0]), Y(0));
      let prevQ = 0;
      for (const [p, q] of pts) { ctx.lineTo(X(p), Y(prevQ)); ctx.lineTo(X(p), Y(q)); prevQ = q; }
      ctx.lineTo(X(pts[pts.length - 1][0]), Y(0));
      ctx.closePath();
      ctx.globalAlpha = 0.25;
      ctx.fillStyle = css(color);
      ctx.fill();
      ctx.globalAlpha = 1;
      ctx.strokeStyle = css(color);
      ctx.lineWidth = 1.5;
      ctx.stroke();
    };
    area(b, "--bid");
    area(a, "--ask");
    ctx.fillStyle = css("--muted");
    ctx.textBaseline = "top";
    ctx.textAlign = "left";
    ctx.fillText(pmin.toFixed(2), pad.l, h - pad.b + 5);
    ctx.textAlign = "right";
    ctx.fillText(pmax.toFixed(2), w - pad.r, h - pad.b + 5);
    ctx.textAlign = "center";
    ctx.fillText(`${((this.bids[0][0] + this.asks[0][0]) / 2).toFixed(3)} mid`, w / 2, h - pad.b + 5);
  }
}

export { fmtNum };
