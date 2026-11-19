/* Procedural Icewind Dale sky + weather renderer.
 * The app sets a target parameter vector and a time of day; the scene eases every
 * parameter toward its target so weather changes roll in rather than snap.
 */
(function (root) {
  'use strict';

  const PARAM_KEYS = ['cloud', 'cloudDark', 'snow', 'flake', 'wind', 'hail', 'fog', 'whiteout',
    'lightning', 'vortex', 'stars', 'moon', 'aurora', 'dark', 'extinguish', 'cold'];

  // Visual signature per weather event id.
  const LOOKS = {
    1:  { cloud: 0.04, cloudDark: 0.1, snow: 0,    flake: 0.5, wind: 0.02, hail: 0, fog: 0,    whiteout: 0,    lightning: 0, vortex: 0, stars: 1,    moon: 0.5,  dark: 0,    extinguish: 0 },
    2:  { cloud: 0.38, cloudDark: 0.2, snow: 0,    flake: 0.5, wind: 0.2,  hail: 0, fog: 0,    whiteout: 0,    lightning: 0, vortex: 0, stars: 0.6,  moon: 0.75, dark: 0.1,  extinguish: 0 },
    3:  { cloud: 0.92, cloudDark: 0.5, snow: 0,    flake: 0.5, wind: 0.42, hail: 0, fog: 0.1,  whiteout: 0,    lightning: 0, vortex: 0, stars: 0,    moon: 0,    dark: 0.62, extinguish: 0 },
    4:  { cloud: 1,    cloudDark: 0.9, snow: 0.06, flake: 0.6, wind: 0.75, hail: 0, fog: 0.05, whiteout: 0,    lightning: 0, vortex: 0, stars: 0,    moon: 0,    dark: 0.82, extinguish: 0 },
    5:  { cloud: 0.42, cloudDark: 0.15, snow: 0.24, flake: 0.35, wind: 0.03, hail: 0, fog: 0.05, whiteout: 0, lightning: 0, vortex: 0, stars: 0.45, moon: 1,    dark: 0.05, extinguish: 0 },
    6:  { cloud: 0.96, cloudDark: 0.55, snow: 0.72, flake: 1, wind: 0.22, hail: 0, fog: 0.15, whiteout: 0.05, lightning: 0, vortex: 0, stars: 0,    moon: 0,    dark: 0.7,  extinguish: 0 },
    7:  { cloud: 1,    cloudDark: 0.7, snow: 1,    flake: 0.75, wind: 0.68, hail: 0, fog: 0.2, whiteout: 0.42, lightning: 0, vortex: 0, stars: 0,    moon: 0,    dark: 0.72, extinguish: 1 },
    8:  { cloud: 1,    cloudDark: 0.1, snow: 1,    flake: 0.6, wind: 1,    hail: 0, fog: 0.4,  whiteout: 0.92, lightning: 0, vortex: 0, stars: 0,    moon: 0,    dark: 0.35, extinguish: 1 },
    9:  { cloud: 0.25, cloudDark: 0.1, snow: 0,    flake: 0.5, wind: 0,    hail: 0, fog: 1,    whiteout: 0.1,  lightning: 0, vortex: 0, stars: 0.12, moon: 1,    dark: 0.15, extinguish: 0 },
    10: { cloud: 0.9,  cloudDark: 0.85, snow: 0.04, flake: 0.5, wind: 0.2, hail: 0, fog: 0,    whiteout: 0,    lightning: 1, vortex: 0, stars: 0,    moon: 0,    dark: 0.6,  extinguish: 0 },
    11: { cloud: 1,    cloudDark: 0.8, snow: 0.1,  flake: 0.5, wind: 0.82, hail: 1, fog: 0.05, whiteout: 0.05, lightning: 0, vortex: 0, stars: 0,    moon: 0,    dark: 0.78, extinguish: 0 },
    12: { cloud: 0.3,  cloudDark: 0.3, snow: 0.3,  flake: 0.4, wind: 1,    hail: 0, fog: 0,    whiteout: 0.1,  lightning: 0, vortex: 1, stars: 0.5,  moon: 0.5,  dark: 0.2,  extinguish: 1 },
  };

  const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
  const lerp = (a, b, t) => a + (b - a) * t;
  const smooth = (t) => t * t * (3 - 2 * t);
  function mixColor(a, b, t) {
    return [lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t)];
  }
  const rgb = (c, a) => `rgba(${c[0] | 0},${c[1] | 0},${c[2] | 0},${a == null ? 1 : a})`;

  // Deterministic decorative RNG so the landscape is stable between reloads.
  function prng(seed) {
    let s = seed >>> 0;
    return () => {
      s = (s + 0x6d2b79f5) >>> 0;
      let t = s;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }

  // Hour-of-day light levels taken from the PDF's light cycles, softened at the edges.
  function band(h, from, to, fade) {
    // wrapping window [from,to) with soft edges `fade` hours wide, centred on each edge
    const len = (((to - from) % 24) + 24) % 24 || 24;
    const x = (((h - from) % 24) + 24) % 24;
    const d = x < len ? Math.min(x, len - x) : -Math.min(x - len, 24 - x);
    return smooth(clamp(0.5 + d / fade, 0, 1));
  }

  function Scene(canvas, frostCanvas) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.frostCanvas = frostCanvas;
    this.frostCtx = frostCanvas ? frostCanvas.getContext('2d') : null;
    this.params = {};
    this.target = {};
    PARAM_KEYS.forEach((k) => { this.params[k] = 0; this.target[k] = 0; });
    this.time = 8;
    this.t = 0;
    this.bolt = null;
    this.flash = 0;
    this.nextStrike = 2;
    this.onThunder = null;
    this.tau = 1.6;            // easing time constant (s); the app shortens it during playback
    this.moonPhase = 0.5;      // shown phase, decoupled from the eased moon brightness
    this._moonVis = 0;
    this.snowOn = 0; this.hailOn = 0;
    const mq = root.matchMedia && root.matchMedia('(prefers-reduced-motion: reduce)');
    this.reduced = !!(mq && mq.matches);
    if (mq) {
      const onMq = (e) => { this.reduced = e.matches; };
      if (mq.addEventListener) mq.addEventListener('change', onMq);
      else if (mq.addListener) mq.addListener(onMq);
    }
    this._initStatic();
    this.resize();
    let pend = 0;
    root.addEventListener('resize', () => {
      if (!pend) pend = requestAnimationFrame(() => { pend = 0; this.resize(); });
    });
    this._last = performance.now();
    const loop = (now) => {
      const dt = Math.min(0.05, (now - this._last) / 1000);
      this._last = now;
      this.update(dt);
      this.draw();
      requestAnimationFrame(loop);
    };
    requestAnimationFrame(loop);
  }

  Scene.LOOKS = LOOKS;
  Scene.PARAM_KEYS = PARAM_KEYS;

  // Blend visual looks around event boundaries so the sky reads as a continuous process.
  Scene.paramsFor = function (segmentsAt, t) {
    // segmentsAt(hour) -> event id, may look into adjacent days for hour <0 or >=24
    const base = Math.floor(t);
    const frac = t - base;
    const a = LOOKS[segmentsAt(base)];
    const b = LOOKS[segmentsAt(base + 1)];
    const p = segmentsAt(base - 1) !== segmentsAt(base) ? LOOKS[segmentsAt(base - 1)] : a;
    const out = {};
    // Weather that changes at the next hour starts building in during the last 40% of this hour;
    // weather that just changed keeps a fading echo of the last one through the first 25%.
    const wIn = frac > 0.6 ? smooth((frac - 0.6) / 0.4) * 0.5 : 0;
    const wOut = frac < 0.25 ? (1 - smooth(frac / 0.25)) * 0.5 : 0;
    Object.keys(a).forEach((k) => {
      let v = a[k];
      if (b !== a) v = lerp(v, b[k], wIn);
      if (p !== a) v = lerp(v, p[k], wOut);
      out[k] = v;
    });
    return out;
  };

  Scene.prototype._initStatic = function () {
    const r = prng(77031);
    this.stars = [];
    for (let i = 0; i < 420; i++) {
      this.stars.push({ x: r(), y: Math.pow(r(), 1.6) * 0.72, s: r() * 1.4 + 0.3, tw: r() * Math.PI * 2, sp: 0.6 + r() * 2.5, hue: r() });
    }
    // Mountain ridgelines (the Spine of the World on the horizon, closer hills in front)
    this.ridges = [0, 1, 2].map((layer) => {
      const pts = [];
      const n = 90;
      const amp = [0.2, 0.12, 0.06][layer];
      const base = [0.6, 0.7, 0.8][layer];
      let phase = r() * 100;
      for (let i = 0; i <= n; i++) {
        const x = i / n;
        const y = base - amp * (0.55 * Math.abs(Math.sin(x * (3 + layer) + phase)) +
          0.3 * Math.abs(Math.sin(x * (9 + layer * 2) + phase * 1.7)) +
          0.15 * Math.sin(x * 23 + phase * 0.3));
        pts.push([x, y]);
      }
      return pts;
    });
    // Clouds: puffs of soft blobs
    this.clouds = [];
    for (let i = 0; i < 26; i++) {
      const puffs = [];
      const count = 5 + Math.floor(r() * 6);
      for (let j = 0; j < count; j++) puffs.push({ dx: (r() - 0.5) * 1.6, dy: (r() - 0.5) * 0.35, r: 0.35 + r() * 0.5 });
      const order = r();
      this.clouds.push({ x: r() * 1.4 - 0.2, y: 0.05 + r() * 0.42, w: 0.12 + r() * 0.2, speed: 0.4 + r() * 0.8, puffs, order,
        ext: Math.max(...puffs.map((pf) => Math.abs(pf.dx) + pf.r)),
        // a few quantised shades so overlapping masses separate without bloating the puff cache
        shade: Math.round(order * 4) / 4 });
    }
    this.clouds.sort((a, b) => a.order - b.order);
    // Precipitation pool; each particle has a depth (z) that scales size/speed.
    // `on` latches only at respawn (or fades in), so weather falls in and out instead of popping.
    this.flakes = [];
    for (let i = 0; i < 1400; i++) {
      this.flakes.push({ x: r(), y: r(), z: 0.25 + r() * 0.75, sway: r() * Math.PI * 2, gate: r(), on: false, a: 0 });
    }
    this.hail = [];
    for (let i = 0; i < 380; i++) this.hail.push({ x: r(), y: r(), z: 0.4 + r() * 0.6, gate: r(), bounce: 0, on: false, vx: 0, vy: 1 });
    this.vortexMotes = [];
    for (let i = 0; i < 600; i++) this.vortexMotes.push({ a: r() * Math.PI * 2, h: r(), rad: r(), sp: 0.6 + r() * 1.4, gate: r(), on: false, fa: 0 });
    // Ten-Towns style huddle of lit huts on the middle ground
    this.huts = [];
    for (let i = 0; i < 9; i++) this.huts.push({ x: 0.62 + i * 0.022 + r() * 0.01, w: 0.012 + r() * 0.01, h: 0.012 + r() * 0.014, lit: r() > 0.25, flick: r() * 10 });
    this.fogBanks = [];
    for (let i = 0; i < 7; i++) this.fogBanks.push({ y: 0.45 + r() * 0.5, h: 0.12 + r() * 0.25, x: r() * 2, sp: 0.01 + r() * 0.03 });
    this.windPhase = 0;
    this.vortexX = 0.3;
  };

  Scene.prototype.resize = function () {
    const dpr = Math.min(root.devicePixelRatio || 1, 1.5);
    const w = root.innerWidth, h = root.innerHeight;
    this.w = w; this.h = h; this.dpr = dpr;
    this.canvas.width = Math.round(w * dpr);
    this.canvas.height = Math.round(h * dpr);
    this.canvas.style.width = w + 'px';
    this.canvas.style.height = h + 'px';
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    if (!this.frostCanvas) return;
    if (this._frostW == null) { this._buildFrost(); return; }
    // small changes (mobile toolbars) just stretch the existing frost; real changes rebuild once settled
    if (dpr === this._frostDpr && Math.abs(w - this._frostW) < 1 && Math.abs(h - this._frostH) < 160) return;
    clearTimeout(this._frostT);
    this._frostT = setTimeout(() => this._buildFrost(), 150);
  };

  // Wrapping strip of soft fog blobs, two screens wide at low resolution, so banks can drift.
  Scene.prototype._buildFogStrip = function () {
    const sw = Math.max(64, Math.round(this.w * 0.5)), sh = Math.max(24, Math.round(this.h * 0.1));
    const c = document.createElement('canvas');
    c.width = sw; c.height = sh;
    const g = c.getContext('2d');
    const grad = g.createRadialGradient(0, 0, 0, 0, 0, 1);
    grad.addColorStop(0, 'rgba(255,255,255,0.5)');
    grad.addColorStop(1, 'rgba(255,255,255,0)');
    g.fillStyle = grad;
    const r = prng(9157);
    for (let i = 0; i < 24; i++) {
      const bx = r() * sw, by = sh * (0.4 + r() * 0.2);
      const rx = sw * (0.04 + r() * 0.07), ry = sh * (0.2 + r() * 0.18);
      g.globalAlpha = 0.5 + r() * 0.5;
      [-sw, 0, sw].forEach((o) => {
        g.setTransform(rx, 0, 0, ry, bx + o, by);
        g.fillRect(-1, -1, 2, 2);
      });
    }
    g.setTransform(1, 0, 0, 1, 0, 0);
    g.globalAlpha = 1;
    const tint = document.createElement('canvas');
    tint.width = sw; tint.height = sh;
    this._fogStrip = c; this._fogTint = tint; this._fogKey = null;
    this._fogW = this.w; this._fogH = this.h;
  };

  // The strip recoloured to the current fog tint; redone only when the quantised colour changes.
  Scene.prototype._fogTinted = function (col) {
    if (!this._fogStrip || this._fogW !== this.w || this._fogH !== this.h) this._buildFogStrip();
    const key = col.map((v) => Math.round(v / 4)).join(',');
    if (key !== this._fogKey) {
      this._fogKey = key;
      const t = this._fogTint, g = t.getContext('2d');
      g.globalCompositeOperation = 'copy';
      g.drawImage(this._fogStrip, 0, 0);
      g.globalCompositeOperation = 'source-in';
      g.fillStyle = rgb(col);
      g.fillRect(0, 0, t.width, t.height);
      g.globalCompositeOperation = 'source-over';
    }
    return this._fogTint;
  };

  // Funnel haze rendered once per size, with a vertical mask so its top dissolves into the sky.
  Scene.prototype._funnel = function () {
    const W = this.w, H = this.h;
    if (this._fun && this._funW === W && this._funH === H) return this._fun;
    const bw = W * 0.34, bh = H * 0.71;
    const c = document.createElement('canvas');
    c.width = Math.max(8, Math.round(bw * 0.5)); c.height = Math.max(8, Math.round(bh * 0.5));
    const g = c.getContext('2d');
    g.scale(c.width / bw, c.height / bh);
    const cx = bw / 2;
    const hg = g.createLinearGradient(cx - W * 0.12, 0, cx + W * 0.12, 0);
    hg.addColorStop(0, 'rgba(210,225,240,0)');
    hg.addColorStop(0.5, 'rgba(210,225,240,0.18)');
    hg.addColorStop(1, 'rgba(210,225,240,0)');
    g.fillStyle = hg;
    g.beginPath();
    g.moveTo(cx - W * 0.02, bh);
    g.quadraticCurveTo(cx - W * 0.05, H * 0.3, cx - W * 0.16, 0);
    g.lineTo(cx + W * 0.16, 0);
    g.quadraticCurveTo(cx + W * 0.05, H * 0.3, cx + W * 0.02, bh);
    g.closePath();
    g.fill();
    g.globalCompositeOperation = 'destination-in';
    const vg = g.createLinearGradient(0, 0, 0, bh);
    vg.addColorStop(0, 'rgba(0,0,0,0)');
    vg.addColorStop(0.12, 'rgba(0,0,0,0.35)');
    vg.addColorStop(0.3, 'rgba(0,0,0,1)');
    vg.addColorStop(0.94, 'rgba(0,0,0,1)');
    vg.addColorStop(1, 'rgba(0,0,0,0.2)');
    g.fillStyle = vg;
    g.fillRect(0, 0, bw, bh);
    this._fun = c; this._funW = W; this._funH = H;
    return c;
  };

  // Soft cloud puff tinted to a colour; cached so slowly-changing tints stay cheap.
  Scene.prototype._puffFor = function (col) {
    const q = col.map((v) => clamp(Math.round(v / 6) * 6, 0, 255));
    const key = q.join(",");
    if (!this._puffs) this._puffs = new Map();
    let c = this._puffs.get(key);
    if (c) return c;
    if (this._puffs.size > 96) this._puffs.clear();
    const s = 128;
    c = document.createElement("canvas");
    c.width = c.height = s;
    const g = c.getContext("2d");
    const grad = g.createRadialGradient(s / 2, s / 2, 0, s / 2, s / 2, s / 2);
    grad.addColorStop(0, rgb(q, 1));
    grad.addColorStop(0.45, rgb(q, 0.55));
    grad.addColorStop(1, rgb(q, 0));
    g.fillStyle = grad;
    g.fillRect(0, 0, s, s);
    this._puffs.set(key, c);
    return c;
  };

  // Frost dendrites creeping from the screen edges; drawn once, faded by coldness.
  Scene.prototype._buildFrost = function () {
    if (!this.frostCanvas) return;
    const fc = this.frostCanvas, g = this.frostCtx;
    const dpr = this.dpr;
    fc.width = Math.round(this.w * dpr);
    fc.height = Math.round(this.h * dpr);
    fc.style.width = '100%';
    fc.style.height = '100%';
    this._frostW = this.w; this._frostH = this.h; this._frostDpr = dpr;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, this.w, this.h);
    const r = prng(4242);
    const W = this.w, H = this.h;
    g.lineCap = 'round';
    const branch = (x, y, ang, len, width, depth) => {
      if (depth <= 0 || len < 2) return;
      const x2 = x + Math.cos(ang) * len, y2 = y + Math.sin(ang) * len;
      g.strokeStyle = `rgba(225,245,255,${0.08 + depth * 0.05})`;
      g.lineWidth = width;
      g.beginPath(); g.moveTo(x, y); g.lineTo(x2, y2); g.stroke();
      const kids = 2 + (r() > 0.6 ? 1 : 0);
      for (let i = 0; i < kids; i++) {
        const t = 0.3 + r() * 0.7;
        const bx = x + (x2 - x) * t, by = y + (y2 - y) * t;
        const side = i % 2 ? 1 : -1;
        branch(bx, by, ang + side * (Math.PI / 3 + (r() - 0.5) * 0.12), len * (0.3 + r() * 0.3), width * 0.65, depth - 1);
      }
      branch(x2, y2, ang + (r() - 0.5) * 0.12, len * 0.72, width * 0.8, depth - 1);
    };
    const seeds = Math.round((W + H) / 38);
    for (let i = 0; i < seeds; i++) {
      const edge = i % 4;
      let x, y, ang;
      if (edge === 0) { x = r() * W; y = 0; ang = Math.PI / 2; }
      else if (edge === 1) { x = r() * W; y = H; ang = -Math.PI / 2; }
      else if (edge === 2) { x = 0; y = r() * H; ang = 0; }
      else { x = W; y = r() * H; ang = Math.PI; }
      // corners grow more frost
      const corner = Math.min(Math.hypot(x, y), Math.hypot(W - x, y), Math.hypot(x, H - y), Math.hypot(W - x, H - y));
      const boost = clamp(1.6 - corner / (Math.min(W, H) * 0.6), 0.5, 1.6);
      branch(x, y, ang + (r() - 0.5) * 0.9, (20 + r() * 50) * boost, 1.4, 5);
    }
    // milky rime at the corners
    const corners = [[0, 0], [W, 0], [0, H], [W, H]];
    corners.forEach(([cx, cy]) => {
      const rad = Math.min(W, H) * 0.5;
      const gr = g.createRadialGradient(cx, cy, 0, cx, cy, rad);
      gr.addColorStop(0, 'rgba(220,240,255,0.22)');
      gr.addColorStop(1, 'rgba(220,240,255,0)');
      g.fillStyle = gr;
      g.fillRect(0, 0, W, H);
    });
  };

  Scene.prototype.setTarget = function (p) {
    PARAM_KEYS.forEach((k) => { if (p[k] != null) this.target[k] = p[k]; });
  };
  Scene.prototype.setTime = function (h) { this.time = h; };
  Scene.prototype.snap = function () {
    const tg = this.target;
    PARAM_KEYS.forEach((k) => { this.params[k] = tg[k]; });
    if (tg.moon > 0.05) this.moonPhase = clamp(tg.moon, 0, 1);
    this.flakes.forEach((f) => { f.on = f.gate < tg.snow; f.a = 1; });
    this.hail.forEach((s) => { s.on = s.gate < tg.hail; });
    this.vortexMotes.forEach((m) => { m.on = m.gate < tg.vortex; m.fa = 1; });
  };

  Scene.prototype.update = function (dt) {
    this.t += dt;
    const ease = 1 - Math.exp(-dt / (this.tau > 0 ? this.tau : 1.6));
    PARAM_KEYS.forEach((k) => { this.params[k] += (this.target[k] - this.params[k]) * ease; });
    const p = this.params;
    const motion = this.reduced ? 0.3 : 1;
    this.windPhase += dt * (0.2 + p.wind * 2.2) * motion;
    const windPx = (0.02 + p.wind * 0.9) * motion; // fraction of width / s

    // The moon's phase keeps its own slow clock: it jumps only while the moon is hidden,
    // otherwise it creeps, so it never visibly waxes or wanes within seconds.
    const mt = this.target.moon;
    if (mt > 0.05) {
      if (this._moonVis < 0.02) this.moonPhase = clamp(mt, 0, 1);
      else this.moonPhase += (clamp(mt, 0, 1) - this.moonPhase) * (1 - Math.exp(-dt / 45));
    }

    // clouds drift with the wind; low scud races ahead of the high deck
    this.clouds.forEach((c) => {
      c.x += dt * (0.004 + windPx * (c.order > 0.55 ? 0.15 : 0.06)) * c.speed;
      const m = c.ext * c.w * 1.8; // 1.8 = largest cover scale, so the whole cloud is off screen
      if (c.x - m > 1) c.x = -m;
    });

    // snow
    const fall = (0.05 + p.flake * 0.05 + p.wind * 0.08) * motion;
    let snowOn = 0;
    this.flakes.forEach((f) => {
      f.sway += dt * (1 + f.z);
      f.y += dt * fall * (0.4 + f.z) * (1 - p.vortex * 0.6);
      f.x += dt * (windPx * (0.5 + f.z) * 0.9 + Math.sin(f.sway) * 0.004);
      const want = f.gate < p.snow;
      if (f.y > 1.02) { f.y = -0.02; f.x = Math.random(); f.on = want; f.a = 0; }
      else if (want) f.on = true;
      if (f.on) {
        // flakes fade in where they are; unwanted ones keep falling while they thin out over ~2 s
        f.a = want ? Math.min(1, f.a + dt * 1.5) : f.a - dt * 0.5;
        if (f.a <= 0) { f.on = false; f.a = 0; } else snowOn++;
      }
      if (f.x > 1.05) f.x -= 1.1;
      if (f.x < -0.05) f.x += 1.1;
    });
    this.snowOn = snowOn;

    // hail is driven by the wind too; stones join or leave only when they respawn above the screen
    let hailOn = 0;
    this.hail.forEach((s) => {
      s.vx = windPx * 0.7;
      s.vy = (0.9 + s.z * 0.9) * motion;
      s.y += dt * s.vy;
      s.x += dt * s.vx;
      if (s.y > 0.86 + s.z * 0.12) {
        s.bounce = s.on ? 0.18 : 0; s.by = s.y; s.bx = s.x;
        s.on = s.gate < p.hail;
        const drift = s.vx / s.vy; // spawn upwind so the left edge isn't bare in a gale
        s.y = -0.05 - Math.random() * 0.3;
        s.x = Math.random() * (1 + drift) - drift;
      }
      if (s.bounce > 0) s.bounce -= dt;
      if (s.x > 1.05) s.x -= 1.1;
      if (s.on || s.bounce > 0) hailOn++;
    });
    this.hailOn = hailOn;

    // vortex column wanders across the dale
    this.vortexX = 0.5 + Math.sin(this.t * 0.07) * 0.28;
    this.vortexMotes.forEach((m) => {
      m.a += dt * m.sp * (1.5 + p.wind * 3) * motion;
      m.h += dt * 0.05 * m.sp * motion;
      const want = m.gate < p.vortex;
      if (m.h > 1) { m.h -= 1; m.on = want; m.fa = 0; }
      else if (want && !m.on) { m.on = true; m.fa = 0; }
      m.fa = m.on ? Math.min(1, m.fa + dt) : 0;
    });

    // fog banks roll along with the wind
    this.fogBanks.forEach((b) => { b.x += dt * b.sp * (1 + p.wind * 3) * motion; });

    // lightning
    this.flash = Math.max(0, this.flash - dt * 3.2);
    if (this.bolt) { this.bolt.life -= dt; if (this.bolt.life <= 0) this.bolt = null; }
    if (p.lightning > 0.25) {
      this.nextStrike -= dt * p.lightning;
      if (this.nextStrike <= 0) {
        this._strike();
        this.nextStrike = 0.8 + Math.random() * 3.5;
      }
    }
  };

  Scene.prototype._strike = function () {
    const W = this.w, H = this.h;
    const x0 = W * (0.1 + Math.random() * 0.8);
    const groundY = H * (0.72 + Math.random() * 0.12);
    const pts = [[x0, H * 0.08]];
    let x = x0, y = H * 0.08;
    const branches = [];
    while (y < groundY) {
      y += 10 + Math.random() * 26;
      x += (Math.random() - 0.5) * 46;
      pts.push([x, Math.min(y, groundY)]);
      if (Math.random() < 0.18) {
        const b = [[x, y]];
        let bx = x, by = y;
        const dir = Math.random() < 0.5 ? -1 : 1;
        for (let i = 0; i < 5; i++) { bx += dir * (8 + Math.random() * 22); by += 8 + Math.random() * 20; b.push([bx, by]); }
        branches.push(b);
      }
    }
    this.bolt = { pts, branches, life: 0.32, max: 0.32 };
    this.flash = this.reduced ? 0.15 : 1;
    if (this.onThunder) this.onThunder(0.5 + Math.random() * 0.5);
  };

  Scene.prototype.draw = function () {
    const ctx = this.ctx, W = this.w, H = this.h, p = this.params, h = this.time;
    const twi = band(h, 10, 14, 1.6);                 // twilight glow
    this._twi = twi;
    const moonUp = band(h, 20, 6, 1.2);
    const auroraUp = band(h, 22, 2, 0.8);
    const cover = clamp(p.cloud, 0, 1);

    // ---- sky ----
    const nightTop = [3, 6, 16], nightLow = [10, 26, 52];
    const twiTop = [28, 52, 84], twiLow = [168, 196, 214];
    const stormTop = [8, 10, 16], stormLow = [34, 40, 52];
    let top = mixColor(nightTop, twiTop, twi * 0.85);
    let low = mixColor(nightLow, twiLow, twi * 0.85);
    top = mixColor(top, stormTop, cover * p.cloudDark * 0.85);
    low = mixColor(low, stormLow, cover * p.cloudDark * 0.75);
    const dim = 1 - p.dark * 0.55;
    top = top.map((v) => v * dim); low = low.map((v) => v * dim);
    const sky = ctx.createLinearGradient(0, 0, 0, H * 0.82);
    sky.addColorStop(0, rgb(top));
    sky.addColorStop(1, rgb(low));
    ctx.fillStyle = sky;
    ctx.fillRect(0, 0, W, H);

    // twilight rose band at the horizon (the sun never truly rises under Auril's rime)
    if (twi > 0.01) {
      const g = ctx.createRadialGradient(W * 0.5, H * 0.72, 0, W * 0.5, H * 0.72, W * 0.7);
      g.addColorStop(0, `rgba(255,190,200,${0.22 * twi * (1 - cover * 0.8)})`);
      g.addColorStop(1, 'rgba(255,190,200,0)');
      ctx.fillStyle = g;
      ctx.fillRect(0, 0, W, H);
    }

    // ---- stars ----
    const starA = p.stars * (1 - twi * 0.9) * (1 - cover * 0.9);
    if (starA > 0.01) {
      this.stars.forEach((s) => {
        const tw = 0.55 + 0.45 * Math.sin(this.t * s.sp + s.tw);
        const a = starA * tw;
        ctx.fillStyle = s.hue > 0.85 ? `rgba(190,220,255,${a})` : `rgba(255,255,255,${a})`;
        ctx.fillRect(s.x * W, s.y * H, s.s, s.s);
      });
    }

    // ---- aurora ----
    const aurA = p.aurora * auroraUp * (1 - cover * 0.85);
    if (aurA > 0.01) this._drawAurora(aurA);

    // ---- moon ----
    const moonA = p.moon * moonUp * (1 - cover * 0.85);
    this._moonVis = moonA;
    if (moonA > 0.01) {
      // moon arcs from east (20:00) to west (06:00)
      const prog = ((h - 20 + 24) % 24) / 10;
      const mx = W * (0.85 - prog * 0.7);
      const my = H * (0.42 - Math.sin(clamp(prog, 0, 1) * Math.PI) * 0.3);
      const mr = Math.max(18, Math.min(W, H) * 0.038);
      const halo = ctx.createRadialGradient(mx, my, mr * 0.6, mx, my, mr * 6);
      halo.addColorStop(0, `rgba(200,225,255,${0.35 * moonA})`);
      halo.addColorStop(1, 'rgba(200,225,255,0)');
      ctx.fillStyle = halo;
      ctx.fillRect(mx - mr * 6, my - mr * 6, mr * 12, mr * 12);
      ctx.save();
      ctx.globalAlpha = moonA;
      ctx.fillStyle = '#eef6ff';
      ctx.beginPath(); ctx.arc(mx, my, mr, 0, Math.PI * 2); ctx.fill();
      // phase shadow: the PDF's ½ / ¾ / full moon visibility
      const lit = clamp(this.moonPhase, 0, 1);
      if (lit < 0.97) {
        ctx.beginPath(); ctx.arc(mx, my, mr, 0, Math.PI * 2); ctx.clip();
        ctx.fillStyle = rgb(top.map((v) => v * 0.9));
        ctx.beginPath();
        ctx.arc(mx - mr * 2 * lit + mr * 0.1, my, mr * 1.02, 0, Math.PI * 2);
        ctx.fill();
      }
      ctx.restore();
      ctx.globalAlpha = moonA * 0.25;
      ctx.fillStyle = '#c9d8ee';
      [[-0.3, -0.2, 0.18], [0.25, 0.15, 0.12], [-0.05, 0.35, 0.1]].forEach(([dx, dy, rr]) => {
        ctx.beginPath(); ctx.arc(mx + dx * mr, my + dy * mr, rr * mr, 0, Math.PI * 2); ctx.fill();
      });
      ctx.globalAlpha = 1;
    }

    // ---- lightning flash lights the sky from within ----
    if (this.flash > 0) {
      ctx.fillStyle = `rgba(200,190,255,${this.flash * 0.35})`;
      ctx.fillRect(0, 0, W, H);
    }

    // ---- clouds (back layer) ----
    this._drawClouds(cover, top, low, 0, 0.55);

    // ---- mountains ----
    const lightLevel = clamp(0.25 + twi * 0.5 + moonA * 0.35 + this.flash * 0.6 - p.dark * 0.3, 0.05, 1);
    this.ridges.forEach((pts, i) => {
      const far = [ [70, 96, 130], [46, 66, 96], [30, 44, 66] ][i];
      const col = far.map((v) => v * (0.5 + lightLevel * 0.8));
      const haze = mixColor(col, low, 0.35 - i * 0.1 + p.fog * 0.4);
      ctx.fillStyle = rgb(haze);
      ctx.beginPath();
      ctx.moveTo(0, H);
      pts.forEach(([x, y]) => ctx.lineTo(x * W, y * H));
      ctx.lineTo(W, H);
      ctx.closePath();
      ctx.fill();
      // snow caps catch the light
      ctx.strokeStyle = `rgba(230,244,255,${(0.12 + lightLevel * 0.35) * (1 - i * 0.25)})`;
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      pts.forEach(([x, y], j) => (j ? ctx.lineTo(x * W, y * H) : ctx.moveTo(x * W, y * H)));
      ctx.stroke();
    });

    // ---- tundra ----
    const gy = H * 0.84;
    const ground = ctx.createLinearGradient(0, gy, 0, H);
    const snowCol = [150, 178, 205].map((v) => v * (0.35 + lightLevel * 0.75));
    ground.addColorStop(0, rgb(snowCol));
    ground.addColorStop(1, rgb(snowCol.map((v) => v * 0.55)));
    ctx.fillStyle = ground;
    ctx.beginPath();
    ctx.moveTo(0, H);
    for (let i = 0; i <= 40; i++) {
      const x = i / 40;
      ctx.lineTo(x * W, gy + Math.sin(x * 9 + 1.3) * H * 0.012 + Math.sin(x * 23) * H * 0.005);
    }
    ctx.lineTo(W, H);
    ctx.closePath();
    ctx.fill();

    this._drawHuts(gy, lightLevel);

    // ---- fog ----
    if (p.fog > 0.01) this._drawFog(p.fog, low, lightLevel);

    // ---- clouds (front layer, low scud) ----
    this._drawClouds(cover, top, low, 0.55, 1);

    // ---- vortex ----
    if (p.vortex > 0.01) this._drawVortex(p.vortex, lightLevel);

    // ---- precipitation ----
    if (this.snowOn > 0) this._drawSnow(p.flake, p.wind, lightLevel);
    if (this.hailOn > 0) this._drawHail(lightLevel);

    // ---- lightning bolt ----
    if (this.bolt) this._drawBolt();

    // ---- white out ----
    if (p.whiteout > 0.01) {
      const wo = p.whiteout;
      const v = 200 + lightLevel * 40;
      ctx.fillStyle = `rgba(${v},${v + 8},${v + 14},${wo * 0.78})`;
      ctx.fillRect(0, 0, W, H);
      // streaking gusts
      ctx.strokeStyle = `rgba(255,255,255,${wo * 0.35})`;
      ctx.lineWidth = 1;
      for (let i = 0; i < 40; i++) {
        const y = ((i * 97.13 + this.t * 40) % H);
        const x = ((i * 211.7 + this.t * (600 + i * 9)) % (W + 400)) - 200;
        ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x + 120 + (i % 5) * 40, y + 6); ctx.stroke();
      }
    }

    // ---- darkness & vignette ----
    const vg = ctx.createRadialGradient(W / 2, H * 0.55, Math.min(W, H) * 0.25, W / 2, H * 0.55, Math.max(W, H) * 0.8);
    vg.addColorStop(0, 'rgba(0,4,12,0)');
    vg.addColorStop(1, `rgba(0,4,12,${0.55 + p.dark * 0.25})`);
    ctx.fillStyle = vg;
    ctx.fillRect(0, 0, W, H);

    // ---- frost on the glass ----
    if (this.frostCanvas) {
      this.frostCanvas.style.opacity = clamp(0.3 + p.cold * 0.75, 0, 1).toFixed(3);
    }
  };

  Scene.prototype._drawClouds = function (cover, top, low, from, to) {
    if (cover < 0.01) return;
    const ctx = this.ctx, W = this.w, H = this.h, p = this.params;
    const n = this.clouds.length;
    // moonlit slate at night, pearl in twilight, bruised charcoal in storms; lightning lights them up
    const lightCol = mixColor([62, 74, 94], [188, 198, 214], this._twi || 0);
    const base = mixColor(lightCol, [22, 25, 33], p.cloudDark * 0.9);
    const col = base.map((v) => clamp(v * (1 - p.dark * 0.35) + this.flash * 140, 0, 255));
    // heavy cover becomes a continuous ceiling, with the puffs drawn over it so the masses still read
    if (from === 0 && cover > 0.6) {
      const g = ctx.createLinearGradient(0, 0, 0, H * 0.7);
      g.addColorStop(0, rgb(col, (cover - 0.6) * 1.6));
      g.addColorStop(1, rgb(col, 0));
      ctx.fillStyle = g;
      ctx.fillRect(0, 0, W, H * 0.7);
    }
    const scale = 1 + cover * 0.8;
    const want = n * clamp(cover * 1.1, 0, 1);
    const cnt = Math.ceil(want);
    // each cloud is a shadowed underside plus a paler body, so overlapping masses separate
    const under = this._puffFor(col.map((v) => v * 0.45));
    const underK = clamp(0.2 + p.cloudDark * 0.9, 0, 1); // pale veils barely shadow, storm decks fully
    for (let i = 0; i < cnt; i++) {
      const c = this.clouds[i];
      if (c.order < from || c.order >= to) continue;
      // the additive lift keeps contrast even in near-black storm decks
      const puff = this._puffFor(col.map((v) => v * (0.75 + c.shade * 0.45) + 8 + c.shade * 24));
      const cw = c.w * W * scale;
      // the boundary cloud fades in with cover instead of popping
      const alpha = clamp(cover * 1.2, 0, 1) * (0.4 + c.order * 0.45) * clamp(want - i, 0, 1);
      // clouds sag lower in the sky as cover thickens
      const cy = (c.y * (1 - cover * 0.35)) * H;
      ctx.globalAlpha = alpha * underK;
      c.puffs.forEach((pf) => {
        const r = pf.r * cw;
        const x = (c.x + pf.dx * c.w * scale) * W;
        const y = cy + pf.dy * cw;
        ctx.drawImage(under, x - r, y - r * 0.4, r * 2, r * 1.3);
      });
      ctx.globalAlpha = alpha;
      c.puffs.forEach((pf) => {
        const r = pf.r * cw * 0.9;
        const x = (c.x + pf.dx * c.w * scale) * W;
        const y = cy + pf.dy * cw;
        ctx.drawImage(puff, x - r, y - r * 0.8, r * 2, r * 1.3);
      });
    }
    ctx.globalAlpha = 1;
  };

  // Curtains are painted into a quarter-resolution buffer and upscaled, which melts the
  // individual columns into soft, continuous ribbons.
  Scene.prototype._drawAurora = function (alpha) {
    const SCALE = 0.25;
    const W = Math.max(64, Math.round(this.w * SCALE)), H = Math.max(64, Math.round(this.h * SCALE));
    if (!this._aur || this._aur.width !== W || this._aur.height !== H) {
      this._aur = document.createElement('canvas');
      this._aur.width = W; this._aur.height = H;
    }
    const ctx = this._aur.getContext('2d'), t = this.t;
    ctx.clearRect(0, 0, W, H);
    ctx.save();
    ctx.globalCompositeOperation = 'lighter';
    const curtains = [
      { hue: [80, 255, 190], y: 0.18, amp: 0.06, f: 2.2, sp: 0.18, len: 0.26 },
      { hue: [110, 200, 255], y: 0.26, amp: 0.05, f: 3.1, sp: -0.12, len: 0.2 },
      { hue: [190, 120, 255], y: 0.12, amp: 0.04, f: 1.6, sp: 0.09, len: 0.16 },
    ];
    curtains.forEach((c, ci) => {
      const step = 2;
      // one unit-height gradient per curtain, stretched onto each column by the transform
      const g = ctx.createLinearGradient(0, 0, 0, 1);
      g.addColorStop(0, rgb(c.hue, 0));
      g.addColorStop(0.15, rgb(c.hue, 1));
      g.addColorStop(1, rgb(c.hue, 0));
      ctx.fillStyle = g;
      for (let x = -step; x < W + step; x += step) {
        const nx = x / W;
        const wave = Math.sin(nx * c.f * Math.PI * 2 + t * c.sp * 3 + ci) * 0.6 +
          Math.sin(nx * c.f * 5.3 + t * c.sp * 7) * 0.4;
        const y = (c.y + wave * c.amp) * H;
        const shimmer = 0.5 + 0.5 * Math.sin(nx * 40 + t * 2.5 + ci * 3);
        const len = c.len * H * (0.7 + shimmer * 0.5);
        // irregular ray brightness; columns tile exactly so nothing doubles up into stripes
        const rays = 0.6 + 0.4 * Math.sin(nx * 97 + t + ci * 2) * Math.sin(nx * 41 - t * 0.7);
        ctx.globalAlpha = clamp(alpha * (0.06 + 0.1 * shimmer) * rays, 0, 1);
        ctx.setTransform(1, 0, 0, len, x, y);
        ctx.fillRect(0, 0, step, 1);
      }
    });
    ctx.restore();
    const main = this.ctx;
    main.save();
    main.globalCompositeOperation = 'lighter';
    main.imageSmoothingEnabled = true;
    main.imageSmoothingQuality = 'high';
    main.drawImage(this._aur, 0, 0, this.w, this.h);
    main.restore();
  };

  Scene.prototype._drawHuts = function (gy, lightLevel) {
    const ctx = this.ctx, W = this.w, H = this.h, p = this.params;
    // a low palisade of huts; lanterns gutter out when the winds extinguish open flames
    const flame = clamp(1 - p.extinguish, 0, 1);
    this.huts.forEach((hut) => {
      const x = hut.x * W, w = hut.w * W, h = hut.h * H;
      const y = gy - h + Math.sin(hut.x * 9 + 1.3) * H * 0.012;
      ctx.fillStyle = rgb([18, 24, 36].map((v) => v * (0.7 + lightLevel * 0.6)));
      ctx.fillRect(x, y, w, h);
      ctx.beginPath();
      ctx.moveTo(x - w * 0.15, y); ctx.lineTo(x + w / 2, y - h * 0.7); ctx.lineTo(x + w * 1.15, y);
      ctx.closePath(); ctx.fill();
      // snow on the roof
      ctx.strokeStyle = `rgba(230,240,255,${0.25 + lightLevel * 0.4})`;
      ctx.lineWidth = 1.2;
      ctx.beginPath(); ctx.moveTo(x - w * 0.15, y); ctx.lineTo(x + w / 2, y - h * 0.7); ctx.lineTo(x + w * 1.15, y); ctx.stroke();
      if (hut.lit) {
        const fl = 0.75 + 0.25 * Math.sin(this.t * 9 + hut.flick) * Math.sin(this.t * 3.3 + hut.flick * 2);
        const a = flame * fl * 0.7;
        if (a > 0.02) {
          const wx = x + w * 0.35, wy = y + h * 0.35, ws = Math.max(2, w * 0.3);
          const glow = ctx.createRadialGradient(wx + ws / 2, wy + ws / 2, 0, wx + ws / 2, wy + ws / 2, ws * 6);
          glow.addColorStop(0, `rgba(255,170,80,${0.35 * a})`);
          glow.addColorStop(1, 'rgba(255,170,80,0)');
          ctx.fillStyle = glow;
          ctx.fillRect(wx - ws * 6, wy - ws * 6, ws * 13, ws * 13);
          ctx.fillStyle = `rgba(255,200,120,${a})`;
          ctx.fillRect(wx, wy, ws, ws);
        }
      }
    });
  };

  Scene.prototype._drawFog = function (fog, low, lightLevel) {
    const ctx = this.ctx, W = this.w, H = this.h;
    const col = mixColor(low, [200, 215, 230], 0.5).map((v) => v * (0.55 + lightLevel * 0.5));
    const strip = this._fogTinted(col);
    this.fogBanks.forEach((b) => {
      // the valley floor fills first, then the fog climbs toward the ridges
      const k = clamp(fog * 1.6 - (1 - b.y) * 1.4, 0, 1);
      if (k <= 0) return;
      const y = b.y * H, hh = b.h * H;
      const x = ((b.x % 2) - 2) * W;
      ctx.globalAlpha = 0.6 * k;
      ctx.drawImage(strip, x, y - hh, 2 * W, hh * 2);
      ctx.drawImage(strip, x + 2 * W, y - hh, 2 * W, hh * 2);
    });
    ctx.globalAlpha = 1;
    // the even wash arrives last, once the banks have gathered
    ctx.fillStyle = rgb(col, fog * fog * 0.38);
    ctx.fillRect(0, 0, W, H);
  };

  Scene.prototype._drawSnow = function (flake, wind, lightLevel) {
    const ctx = this.ctx, W = this.w, H = this.h;
    const n = this.flakes.length;
    const bright = 0.55 + lightLevel * 0.45;
    // dots stretch smoothly into streaks as the wind rises
    const streak = clamp(wind * 1.4, 0, 1);
    const dim = lerp(1, 0.8, streak);
    ctx.lineCap = 'round';
    for (let i = 0; i < n; i++) {
      const f = this.flakes[i];
      if (!f.on || f.a <= 0) continue;
      const x = f.x * W, y = f.y * H;
      const size = (0.6 + flake * 2.4) * f.z * 1.6;
      const a = (0.35 + f.z * 0.6) * bright * dim * f.a;
      const len = streak * 22 * f.z;
      ctx.strokeStyle = `rgba(235,245,255,${a})`;
      ctx.lineWidth = lerp(size * 2, size * 0.9, streak);
      ctx.beginPath();
      ctx.moveTo(x, y);
      ctx.lineTo(x - len - 0.01, y - streak * 4 * f.z); // never zero-length, so round caps still draw a dot
      ctx.stroke();
    }
  };

  Scene.prototype._drawHail = function (lightLevel) {
    const ctx = this.ctx, W = this.w, H = this.h;
    const bright = 0.6 + lightLevel * 0.4;
    ctx.lineCap = 'round';
    this.hail.forEach((s) => {
      if (s.on) {
        // the streak trails back along the stone's wind-driven path
        const x = s.x * W, y = s.y * H;
        const L = 18 * s.z, n = Math.hypot(s.vx * W, s.vy * H) || 1;
        ctx.strokeStyle = `rgba(220,235,255,${0.55 * bright * s.z})`;
        ctx.lineWidth = 1.2 + s.z * 1.6;
        ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x - s.vx * W / n * L, y - s.vy * H / n * L); ctx.stroke();
      }
      if (s.bounce > 0 && s.bx != null) {
        const k = s.bounce / 0.18;
        ctx.fillStyle = `rgba(230,240,255,${0.7 * k})`;
        ctx.beginPath();
        ctx.arc(s.bx * W + (1 - k) * 8, s.by * H - Math.sin(k * Math.PI) * 10, 1 + s.z * 1.5, 0, Math.PI * 2);
        ctx.fill();
      }
    });
  };

  Scene.prototype._drawVortex = function (v, lightLevel) {
    const ctx = this.ctx, W = this.w, H = this.h;
    const cx = this.vortexX * W;
    const baseY = H * 0.86, topY = H * 0.15;
    const bright = 0.5 + lightLevel * 0.5;
    // funnel haze (cached, soft-topped)
    ctx.globalAlpha = clamp(v, 0, 1);
    ctx.drawImage(this._funnel(), cx - W * 0.17, topY, W * 0.34, baseY - topY);
    ctx.globalAlpha = 1;
    // spiralling motes; they thin out at the base and top so the wrap is invisible
    this.vortexMotes.forEach((m) => {
      if (!m.on || m.fa <= 0) return;
      const hNorm = m.h;
      const y = baseY - hNorm * (baseY - topY);
      const radius = (0.015 + Math.pow(hNorm, 1.4) * 0.15) * W * (0.6 + m.rad * 0.6);
      const x = cx + Math.cos(m.a) * radius;
      const depth = Math.sin(m.a);
      const edge = clamp(Math.min(hNorm, 1 - hNorm) * 10, 0, 1);
      const a = (0.25 + 0.45 * (depth + 1) / 2) * bright * v * m.fa * edge;
      ctx.fillStyle = `rgba(235,245,255,${a})`;
      const s = 0.8 + (depth + 1) * 0.8;
      ctx.fillRect(x, y + depth * radius * 0.12, s * 2.2, s);
    });
  };

  Scene.prototype._drawBolt = function () {
    const ctx = this.ctx, b = this.bolt;
    const k = b.life / b.max;
    ctx.save();
    ctx.globalCompositeOperation = 'lighter';
    ctx.lineJoin = 'round';
    const stroke = (pts, width, alpha) => {
      ctx.strokeStyle = `rgba(210,200,255,${alpha * k})`;
      ctx.lineWidth = width;
      ctx.beginPath();
      pts.forEach(([x, y], i) => (i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)));
      ctx.stroke();
    };
    ctx.shadowColor = 'rgba(180,160,255,0.9)';
    ctx.shadowBlur = 24;
    stroke(b.pts, 6, 0.25);
    stroke(b.pts, 2.2, 0.95);
    b.branches.forEach((br) => stroke(br, 1.2, 0.6));
    ctx.restore();
  };

  root.IWDScene = Scene;
})(window);
