#!/usr/bin/env python3
"""Regenerates native/assets/w01.jpg .. w12.jpg (1200x400 weather scene images).

Dependencies: numpy + Pillow only (no scipy).
Deterministic: every random stream is seeded with a fixed constant.

Everything is raster and procedural: four different heightfield terrains
(different ranges, camera set-ups and moon positions) are each ray-cast once
into a G-buffer, and every weather event gets one of those views plus its own
sky, cloud deck, lighting, haze and particle effects.  Rendering happens in linear light at 2x supersampling and is
box-filtered down, tone-mapped, grained and saved as baseline JPEG.

The scenes were composed at 720x240.  Every length measured in pixels is
written in those 720-wide units and multiplied by K (= output width / 720), so
any output width renders the same picture, only sharper; --width 720
reproduces the original 720x240 files byte for byte.

Usage:  python3 tools/gen_assets.py [--width N] [event ids ...]
"""
import io
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw


def _width_arg():
    for i, a in enumerate(sys.argv[1:], 1):
        if a == "--width":
            return int(sys.argv[i + 1])
        if a.startswith("--width="):
            return int(a.split("=", 1)[1])
    return 1200


W0 = _width_arg()
H0, S = W0 // 3, 2
W, H = W0 * S, H0 * S
K = W0 / 720.0                  # pixel lengths below are in 720-wide units


def px(v):
    """A pixel length from the 720x240 composition, at the output resolution."""
    return v * K


def ipx(v):
    return int(round(v * K))

HOR = int(H * 0.675)            # horizon row (supersampled); set per view
FOCAL = W * 1.15                # set per view
F32 = np.float32
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "native", "assets")
MAX_BYTES = 58000 if W0 <= 720 else 100000    # 12 x 100 KB stays under 1.2 MB

YY, XX = np.mgrid[0:H, 0:W].astype(F32)
YY += 0.5
XX += 0.5

# moon position on screen (supersampled px) and light direction on the map
MOON_X, MOON_Y = 0.60 * W, 0.20 * H


def srgb(r, g, b):
    return (np.array([r, g, b], F32) / 255.0) ** 2.2


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


# --------------------------------------------------------------------------
# noise
# --------------------------------------------------------------------------
def spectral(n, seed, f_lo, f_hi, beta):
    """Tileable gaussian noise with a 1/f^beta spectrum between two cut-offs."""
    rng = np.random.default_rng(seed)
    F = np.fft.rfft2(rng.standard_normal((n, n)))
    fy = np.fft.fftfreq(n)[:, None] * n
    fx = np.fft.rfftfreq(n)[None, :] * n
    f = np.hypot(fx, fy)
    f[0, 0] = 1.0
    amp = f ** (-beta / 2.0)
    amp *= np.exp(-(f / f_hi) ** 4) * (1.0 - np.exp(-(f / f_lo) ** 4))
    amp[0, 0] = 0.0
    out = np.fft.irfft2(F * amp, s=(n, n))
    return (out / out.std()).astype(F32)


def bandnoise(n, seed, f0, width=0.45):
    rng = np.random.default_rng(seed)
    F = np.fft.rfft2(rng.standard_normal((n, n)))
    fy = np.fft.fftfreq(n)[:, None] * n
    fx = np.fft.rfftfreq(n)[None, :] * n
    f = np.hypot(fx, fy)
    f[0, 0] = 1e-6
    amp = np.exp(-0.5 * (np.log2(f / f0) / width) ** 2)
    out = np.fft.irfft2(F * amp, s=(n, n))
    return (out / out.std()).astype(F32)


def bil(tex, x, y):
    """Bilinear lookup with wrap-around, x/y in texels."""
    h, w = tex.shape
    x0 = np.floor(x)
    y0 = np.floor(y)
    fx = (x - x0).astype(F32)
    fy = (y - y0).astype(F32)
    xi = x0.astype(np.intp)
    xi %= w
    yi = y0.astype(np.intp)
    yi %= h
    del x0, y0
    x1 = xi + 1
    x1[x1 == w] = 0
    yi *= w                                    # flat row offsets
    y1 = yi + w
    y1[y1 == h * w] = 0
    t = tex.ravel()
    top = t.take(yi + x1)
    a = t.take(yi + xi)
    top -= a
    top *= fx
    top += a
    bot = t.take(y1 + x1)
    c = t.take(y1 + xi)
    bot -= c
    bot *= fx
    bot += c
    bot -= top
    bot *= fy
    bot += top
    return bot


def bilc(tex, x, y):
    """Bilinear lookup, clamped at the borders."""
    h, w = tex.shape
    x = np.clip(x, 0, w - 1.001)
    y = np.clip(y, 0, h - 1.001)
    x0 = x.astype(np.intp)
    y0 = y.astype(np.intp)
    fx = (x - x0).astype(F32)
    fy = (y - y0).astype(F32)
    a = tex[y0, x0]
    b = tex[y0, x0 + 1]
    c = tex[y0 + 1, x0]
    d = tex[y0 + 1, x0 + 1]
    top = a + (b - a) * fx
    bot = c + (d - c) * fx
    return top + (bot - top) * fy


BASE_N = 256
BASE = spectral(BASE_N, 7, 1.5, 8.0, 2.4)


def fbm(u, v, octaves=6, gain=0.5, seed=0, fp=None, lac=2.07, ridge=False):
    """fBm by re-sampling one smooth tile at rising frequencies.

    u, v are in tile units.  fp (tile units per pixel) fades out octaves that
    would alias (used for perspective-compressed cloud/ground lookups)."""
    rng = np.random.default_rng(900 + seed)
    tot = np.zeros(np.broadcast(u, v).shape, F32)
    amp, f, norm = 1.0, 1.0, 0.0
    for o in range(octaves):
        ang = 0.5 + o * 1.1
        c, s = math.cos(ang), math.sin(ang)
        ox, oy = rng.uniform(0, 1, 2)
        uu = ((u * c - v * s) * f + ox) * BASE_N
        vv = ((u * s + v * c) * f + oy) * BASE_N
        n = bil(BASE, uu, vv)
        if ridge:
            n = np.abs(n) * 1.6 - 1.0
        if fp is not None:
            n = n * np.clip(2.0 - 32.0 * f * fp, 0.0, 1.0)
        tot += amp * n
        norm += amp * amp
        amp *= gain
        f *= lac
    return tot / math.sqrt(norm)


def gblur(img, sigma):
    """Gaussian blur (FFT) with reflect padding; works on HxW or HxWx3."""
    if sigma <= 0:
        return img
    if img.ndim == 3:
        return np.stack([gblur(img[..., i], sigma) for i in range(3)], -1)
    h, w = img.shape
    py = min(int(3 * sigma) + 1, h - 1)
    px = min(int(3 * sigma) + 1, w - 1)
    p = np.pad(img, ((py, py), (px, px)), mode="reflect")
    fy = np.fft.fftfreq(p.shape[0])[:, None]
    fx = np.fft.rfftfreq(p.shape[1])[None, :]
    k = np.exp(-2.0 * (math.pi * sigma) ** 2 * (fx * fx + fy * fy))
    out = np.fft.irfft2(np.fft.rfft2(p) * k, s=p.shape)
    return out[py:py + h, px:px + w].astype(F32)


def splat(img, x, y, a):
    """Additive bilinear point splat."""
    h, w = img.shape
    x = x - 0.5
    y = y - 0.5
    x0 = np.floor(x).astype(np.intp)
    y0 = np.floor(y).astype(np.intp)
    fx = x - x0
    fy = y - y0
    for dx, dy, wt in ((0, 0, (1 - fx) * (1 - fy)), (1, 0, fx * (1 - fy)),
                       (0, 1, (1 - fx) * fy), (1, 1, fx * fy)):
        xi = x0 + dx
        yi = y0 + dy
        ok = (xi >= 0) & (xi < w) & (yi >= 0) & (yi < h)
        np.add.at(img, (yi[ok], xi[ok]), (a * wt)[ok])


def streak_filter(img, sigma, length, angle_deg):
    """Convolve with a gaussian dot stretched into a line (motion blur).
    The kernel is normalised to unit peak, so splat amplitudes are peak values."""
    h, w = img.shape
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.rfftfreq(w)[None, :]
    a = math.radians(angle_deg)
    k = np.exp(-2.0 * (math.pi * sigma) ** 2 * (fx * fx + fy * fy))
    if length > 0:
        k = k * np.sinc(length * (fx * math.cos(a) + fy * math.sin(a)))
    peak = 1.0 / (math.sqrt(2 * math.pi) * sigma *
                  max(length, math.sqrt(2 * math.pi) * sigma))
    return (np.fft.irfft2(np.fft.rfft2(img) * k, s=(h, w)) / peak).astype(F32)


# --------------------------------------------------------------------------
# terrain (four views, each built once and shared by three scenes)
# --------------------------------------------------------------------------
MAPN = 2048
U = 2.0                                   # map cells per world unit
CAMX, CAMY, CAMH = 1024.0, 40.0, 6.4
LIGHT = np.array([0.80, -0.42, 0.42])          # towards the moon (map x, y, up)
LIGHT = LIGHT / np.linalg.norm(LIGHT)


# seed: noise seed offset; edge: distance of the range; mscale: peak height;
# hor: horizon row / H; focal: focal length / W; flip: mirror the view (moon on
# the left); moon: x search window / W for the moon (in the unflipped view)
VIEWS = {
    "A": dict(seed=0, edge=440.0, mscale=92.0, hor=0.675, focal=1.15, flip=False, moon=(0.56, 0.70)),
    "B": dict(seed=1000, edge=560.0, mscale=84.0, hor=0.705, focal=1.05, flip=True, moon=(0.62, 0.78)),
    "C": dict(seed=2000, edge=380.0, mscale=104.0, hor=0.655, focal=1.30, flip=False, moon=(0.30, 0.46)),
    "D": dict(seed=3000, edge=470.0, mscale=96.0, hor=0.690, focal=0.95, flip=True, moon=(0.50, 0.64)),
}
# which view each weather event uses, and how high its moon hangs (fraction of H)
EVENT_VIEW = {1: "A", 7: "A", 11: "A", 2: "B", 6: "B", 10: "B",
              4: "C", 5: "C", 9: "C", 3: "D", 8: "D", 12: "D"}
MOON_H = {1: 0.20, 2: 0.26, 5: 0.15, 3: 0.14, 9: 0.22}


def build_heightmap(V):
    n = MAPN
    so = V["seed"]
    yc = np.arange(n, dtype=F32)[:, None] * np.ones((1, n), F32)
    # ridged multifractal
    h = np.zeros((n, n), F32)
    wgt = np.ones((n, n), F32)
    for o in range(9):
        f = 2.6 * 2.0 ** o
        sig = 1.0 - np.abs(bandnoise(n, 100 + so + o, f)) * 0.62
        sig = np.clip(sig, 0, 1) ** 2 * wgt
        wgt = np.clip(sig * 2.2, 0, 1)
        h += sig * (0.5 ** (o * 0.98))
    h = h / h.max()
    # gentle domain warp to break the isotropy of the noise
    wx = spectral(n, 201 + so, 2, 12, 3.0) * 9.0 * U
    wy = spectral(n, 202 + so, 2, 12, 3.0) * 9.0 * U
    xs = np.arange(n, dtype=F32)[None, :] + wx
    ys = yc + wy
    h = bil(h, xs, ys)
    massif = spectral(n, 203 + so, 1, 5, 3.0)
    yw = yc / U
    edge = V["edge"] + 45.0 * spectral(n, 204 + so, 1, 6, 3.0)
    belt = smooth(edge - 50, edge + 120, yw) * (1.0 - smooth(900, 1010, yw))
    mount = belt * (h ** 1.2) * V["mscale"] * np.clip(0.62 + 0.32 * massif, 0.25, 1.1)
    mount = 60.0 * np.tanh(mount / 60.0)       # soft cap so no spire dominates
    foot = smooth(170, 380, yw) * (1.0 - smooth(930, 1015, yw))
    knoll = np.clip(spectral(n, 205 + so, 5, 50, 2.6) + 0.1, 0, None) ** 1.3
    hills = foot * (knoll * 3.6 + 1.5)
    drift = spectral(n, 206 + so, 12, 110, 2.9) * 0.30 + spectral(n, 207 + so, 5, 16, 3.0) * 0.55
    return ((mount + hills + drift) * U).astype(F32)


def use_view(G):
    """Point the module-level camera globals at one view."""
    global HOR, FOCAL, MOON_X
    HOR, FOCAL, MOON_X = G["hor"], G["focal"], G["moon_x"]


def build_gbuffer(V):
    global HOR, FOCAL
    HOR = int(H * V["hor"])
    FOCAL = W * V["focal"]
    hm = build_heightmap(V)
    n = MAPN
    gy, gx = np.gradient(hm)
    nz = 1.0 / np.sqrt(gx * gx + gy * gy + 1.0)
    diff = np.clip((-gx * LIGHT[0] - gy * LIGHT[1] + LIGHT[2]) * nz, 0, 1)
    slope = np.hypot(gx, gy)
    # cast shadows towards the moon
    occ = np.full((n, n), -1e9, F32)
    lh = math.hypot(LIGHT[0], LIGHT[1])
    dx, dy, dz = LIGHT[0] / lh, LIGHT[1] / lh, LIGHT[2] / lh
    for k in np.arange(2.0, 320, 2.0):
        sh = np.roll(hm, (-int(round(dy * k)), -int(round(dx * k))), (0, 1))
        np.maximum(occ, sh - k * dz, out=occ)
    shadow = gblur(smooth(-0.5, 2.0, occ - hm), 1.5)
    ao = np.clip(0.5 + (hm - gblur(hm, 22)) * 0.045, 0, 1)

    # perspective ray-cast of the heightfield (front-to-back column sweep)
    zs = np.unique(np.concatenate([1.0 / np.linspace(1 / 32.0, 1 / 2000.0, 900),
                                   np.linspace(32.0, 2000.0, 1700)])).astype(F32)
    cols = ((np.arange(W) + 0.5 - W / 2) / FOCAL).astype(F32)
    mx = CAMX + zs[:, None] * cols[None, :]
    my = CAMY + zs[:, None] + np.zeros_like(mx)
    hh = bil(hm, mx, my)
    camz = float(bil(hm, np.array([CAMX]), np.array([CAMY]))[0]) + CAMH
    ytop = HOR - (hh - camz) / zs[:, None] * FOCAL
    cm = np.minimum.accumulate(ytop, axis=0)
    skyline = cm[-1]
    rows = np.arange(H) + 0.5
    idx = np.empty((H, W), np.intp)
    for c in range(W):
        q = np.maximum(rows, skyline[c] + 1e-3)
        idx[:, c] = np.searchsorted(-cm[:, c], -q, side="left")
    idx = np.clip(idx, 0, len(zs) - 1)
    ci = np.arange(W)[None, :] + np.zeros((H, 1), np.intp)
    ip = np.maximum(idx - 1, 0)
    y_a = ytop[ip, ci]
    y_b = ytop[idx, ci]
    q = np.maximum(rows[:, None], skyline[None, :] + 1e-3)
    t = np.where(idx > 0, np.clip((y_a - q) / np.maximum(y_a - y_b, 1e-4), 0, 1), 1.0)
    z = zs[ip] + t * (zs[idx] - zs[ip])
    cov = np.clip(np.arange(H)[:, None] + 1.0 - skyline[None, :], 0, 1).astype(F32)
    # put the moon over the most open stretch of skyline right of centre
    cand = np.arange(int(V["moon"][0] * W), int(V["moon"][1] * W), ipx(8))
    moon_x = float(cand[np.argmax([skyline[c - ipx(60):c + ipx(60)].min() for c in cand])])
    pmx = (CAMX + z * cols[None, :]).astype(F32)
    pmy = (CAMY + z).astype(F32)
    g = {
        "z": (z / U).astype(F32), "cov": cov,
        "h": bil(hm, pmx, pmy) / U, "diff": bil(diff, pmx, pmy),
        "shadow": bil(shadow, pmx, pmy), "slope": bil(slope, pmx, pmy),
        "ao": bil(ao, pmx, pmy), "skyline": skyline.astype(F32),
    }
    pmx, pmy = pmx / U, pmy / U                # world units from here on
    g["mx"], g["my"] = pmx, pmy
    # true per-pixel footprint on the ground (world units per pixel)
    fpx = np.maximum(np.abs(np.gradient(pmy, axis=0)), np.abs(np.gradient(pmx, axis=1)))
    fpx = np.minimum(fpx, 50.0).astype(F32)
    g["fp"] = fpx
    # wind-sculpted snow relief (sastrugi), faded where it would alias
    u, v = pmx / 60.0, pmy / 22.0
    d1 = fbm(u, v, 5, 0.55, 31, fp=fpx / 22.0)
    d2 = fbm(u + 0.02, v - 0.012, 5, 0.55, 31, fp=fpx / 22.0)
    g["relief"] = (d1 - d2).astype(F32)
    g["grainy"] = fbm(pmx / 7.0, pmy / 7.0, 5, 0.65, 32, fp=fpx / 7.0)
    # rock exposure on steep faces, broken up by noise streaked down-slope
    rn = fbm(pmx / 40.0, pmy / 40.0, 6, 0.6, 33, fp=fpx / 40.0)
    g["rock"] = (smooth(1.15, 2.1, g["slope"] + 0.45 * rn) *
                 smooth(8.0, 18.0, g["h"])).astype(F32)
    g["rn"] = rn
    if V["flip"]:
        g = {k: np.ascontiguousarray(a[..., ::-1]) for k, a in g.items()}
        moon_x = W - moon_x
    g["hor"], g["focal"], g["moon_x"] = HOR, FOCAL, moon_x
    return g


# --------------------------------------------------------------------------
# sky elements
# --------------------------------------------------------------------------
def sky_gradient(zen, hor, power=0.55, glow=None, seed=0):
    t = np.clip((HOR + 0.06 * H - YY) / (HOR + 0.06 * H), 0, 1) ** power
    sky = hor[None, None, :] * (1 - t[..., None]) + zen[None, None, :] * t[..., None]
    if glow is not None:
        col, gx, gw, gh = glow
        g = np.exp(-((XX - gx * W) / (gw * W)) ** 2) * np.exp(-np.clip(HOR - YY, 0, None) / (gh * H))
        sky = sky + col[None, None, :] * g[..., None]
    # slow, large-scale unevenness so the gradient is never mathematically clean
    n = fbm(XX / W * 0.5, YY / W * 0.5, 3, 0.5, 40 + seed)
    return (sky * (1.0 + 0.10 * n[..., None])).astype(F32)


def stars_layer(rng, count=1500):
    x = rng.uniform(0, W, count * 3)
    y = rng.uniform(0, HOR, count * 3)
    # milky-way style band: keep more stars near a tilted line
    d = (y - (0.05 * H + 0.42 * (x - 0.1 * W))) / (0.16 * H)
    keep = rng.uniform(0, 1, count * 3) < 0.30 + 0.70 * np.exp(-d * d)
    x, y = x[keep][:count], y[keep][:count]
    mag = np.minimum(rng.pareto(1.9, len(x)) * 0.05 + 0.015, 1.6).astype(F32)
    tc = rng.uniform(-1, 1, len(x))
    out = np.zeros((H, W, 3), F32)
    for i, k in enumerate((0.16, 0.02, -0.18)):
        ch = np.zeros((H, W), F32)
        splat(ch, x, y, mag * (1.0 + k * tc))
        out[..., i] = (gblur(ch, px(0.7)) * 4.5 + gblur(ch * (ch > 0.25), px(2.4)) * 5.0) * K * K
    band = np.exp(-((YY - (0.05 * H + 0.42 * (XX - 0.1 * W))) / (0.13 * H)) ** 2)
    mw = band * np.clip(0.55 + 0.5 * fbm(XX / W * 0.9, YY / W * 0.9, 6, 0.62, 51), 0, None)
    out += mw[..., None] * srgb(24, 28, 42)[None, None, :]
    # extinction towards the horizon
    ext = smooth(0.0, 0.30 * H, HOR - YY) * 0.85 + 0.15
    return out * ext[..., None]


def moon_layer(rng):
    r = px(15.0)
    d = np.hypot(XX - MOON_X, YY - MOON_Y)
    disc = smooth(r + px(1.0), r - px(1.0), d)
    mare = fbm((XX - MOON_X) / px(70.0), (YY - MOON_Y) / px(70.0), 5, 0.6, 61)
    limb = np.sqrt(np.clip(1.0 - (d / r) ** 2, 0, 1))
    tex = (0.80 + 0.20 * limb) * (1.0 - 0.33 * smooth(-0.2, 0.9, mare))
    glow = 0.30 * np.exp(-d / (0.035 * W)) + 0.10 * np.exp(-d / (0.12 * W)) + \
        0.035 * np.exp(-d / (0.35 * W))
    return (disc * tex).astype(F32), glow.astype(F32)


def aurora_layer(rng, curtains):
    """Folded curtains: impulses along a wavy path, smeared upwards."""
    out = np.zeros((H, W, 3), F32)
    hh = 2 * H
    d = np.arange(hh, dtype=F32)
    for ci, (y0, slope, fold, lg, strength, seed) in enumerate(curtains):
        n = 9000
        t = np.linspace(0, 1, n).astype(F32)
        ph = rng.uniform(0, 6.28)
        k = fold
        x = (-0.15 + 1.3 * t) * W + 0.050 * W * np.sin(6.283 * t * k + ph) \
            + 0.05 * W * fbm(t * 0.5, np.full_like(t, 0.13 + seed), 2, 0.5, seed)
        y = y0 * H + slope * (t - 0.5) * H + 0.9 * abs(slope) * H * (t - 0.5) ** 2 \
            + 0.050 * H * np.cos(6.283 * t * k + ph) \
            + 0.085 * H * fbm(t * 0.35, np.full_like(t, 0.57 + seed), 2, 0.5, seed + 1)
        env = np.clip(fbm(t * 0.22, np.full_like(t, 0.31 + seed), 2, 0.5, seed + 2) * 0.55 + 0.75, 0.08, None) ** 1.5
        rays = 0.72 + 0.24 * fbm(t * 3.0, np.full_like(t, 0.77), 3, 0.7, seed + 3) \
            + 0.14 * fbm(t * 14.0, np.full_like(t, 0.21), 2, 0.7, seed + 4)
        rays = np.clip(rays, 0.15, None)
        amp = env * rays * (W / n) * strength
        imp = np.zeros((hh, W), F32)
        splat(imp, x, y + H * 0.5, amp)      # offset so tall rays do not wrap
        IMP = np.fft.rfft(imp, axis=0)
        Lg = lg * H
        kg = np.exp(-d / Lg) + 0.7 * np.exp(-d / (Lg * 3.2))
        kg[1:] += np.exp(-((hh - d[1:]) / px(2.2)) ** 2) + 0.45 * np.exp(-(hh - d[1:]) / (Lg * 0.45))
        kp = (d / (Lg * 1.3)) ** 1.5 * np.exp(-d / (Lg * 1.5))
        g = np.fft.irfft(IMP * np.conj(np.fft.rfft(kg))[:, None], n=hh, axis=0)
        p = np.fft.irfft(IMP * np.conj(np.fft.rfft(kp))[:, None], n=hh, axis=0)
        g = g[H // 2:H // 2 + H].astype(F32)
        p = p[H // 2:H // 2 + H].astype(F32)
        g = gblur(g, px(3.5)) * 0.62 + gblur(g, px(10.0)) * 0.42 + gblur(g, px(34.0)) * 0.22
        p = gblur(p, px(4.0)) * 0.5 + gblur(p, px(16.0)) * 0.6
        out += g[..., None] * np.array([0.07, 0.95, 0.40], F32) + \
            p[..., None] * np.array([0.30, 0.16, 0.62], F32) * 0.55
    return out


def cloud_density(seed, thr, soft, sx=1.0, sy=1.0, warp=0.25, octaves=7, gain=0.55,
                  swirl=None, ch=0.12, billow=0.0):
    """Cloud deck projected on a plane above the camera -> perspective bunching
    towards the horizon.  Returns a 0..1 coverage/thickness field."""
    X, Y = XX, YY
    if swirl is not None:
        cx, cy, ang, rad = swirl
        dx, dy = X - cx, (Y - cy) * 2.2
        a = ang * np.exp(-np.hypot(dx, dy) / rad)
        ca, sa = np.cos(a), np.sin(a)
        X = cx + dx * ca - dy * sa
        Y = cy + (dx * sa + dy * ca) / 2.2
    yh = HOR + 0.10 * H
    dy = np.maximum(yh - Y, px(6.0))
    v = ch * FOCAL / dy
    u = (X - W / 2) * ch / dy
    fp = np.maximum(ch * FOCAL / (dy * dy), ch / dy)
    u, v = u * sx, v * sy
    fp = fp * max(sx, sy)
    if warp:
        wu = fbm(u * 0.35 + 3.1, v * 0.35, 2, 0.5, seed + 1, fp=fp)
        wv = fbm(u * 0.35, v * 0.35 + 7.7, 2, 0.5, seed + 2, fp=fp)
        u = u + warp * 0.10 * wu
        v = v + warp * 0.10 * wv
    n = fbm(u, v, octaves, gain, seed, fp=fp)
    if billow:
        n = n * (1 - billow) + billow * (fbm(u * 2.0, v * 2.0, octaves - 1, 0.6, seed + 5, fp=fp * 2, ridge=True) * 0.9)
    dens = smooth(thr, thr + soft, n)
    return dens.astype(F32), n.astype(F32)


def cloud_light(dens, lx, ly, step=7.0, kappa=1.1):
    """Self-shadowing: march the density field towards the light position."""
    dx, dy = lx - XX, ly - YY
    dl = np.hypot(dx, dy) + 1e-3
    dx, dy = dx / dl, dy / dl
    occ = np.zeros_like(dens)
    for k in (1, 2, 3.5, 5.5):
        occ += bilc(dens, XX - 0.5 + dx * px(step) * k, YY - 0.5 + dy * px(step) * k)
    return np.exp(-kappa * occ)


# --------------------------------------------------------------------------
# particles / effects
# --------------------------------------------------------------------------
def calm_mask():
    """Lower weight in the bottom-left corner where the title text sits."""
    return 1.0 - 0.65 * smooth(0.52 * W, 0.30 * W, XX) * smooth(0.55 * H, 0.80 * H, YY)


def snow(rng, n, amp, sigma, length, angle, ymin=0, calm=False):
    img = np.zeros((H, W), F32)
    x = rng.uniform(0, W, n)
    y = rng.uniform(ymin, H, n)
    a = rng.uniform(0.25, 1.0, n) ** 2 * amp
    splat(img, x, y, a)
    out = streak_filter(img, px(sigma), px(length), angle)
    if calm:
        out = out * calm_mask()
    return np.clip(out, 0, None)


def warp(img, ax, ay, scale, seed):
    dx = fbm(XX / W * scale, YY / W * scale, 2, 0.4, seed) * px(ax)
    dy = fbm(XX / W * scale + 5.0, YY / W * scale, 2, 0.4, seed + 1) * px(ay)
    return bil(img, XX - 0.5 + dx, YY - 0.5 + dy)


def over(img, a, col):
    a = np.clip(a, 0, 1)[..., None]
    return img * (1 - a) + col[None, None, :] * a


def wind_sheets(seed, angle_deg, stretch, scale, octaves=6, gain=0.6):
    """Anisotropic fBm stretched along the wind direction (blown snow)."""
    a = math.radians(angle_deg)
    c, s = math.cos(a), math.sin(a)
    al = (XX * c + YY * s) / W
    ac = (-XX * s + YY * c) / W
    wob = fbm(al * 0.4, ac * 0.4, 2, 0.5, seed + 9) * 0.03
    return fbm(al * scale / stretch, (ac + wob) * scale, octaves, gain, seed)


def lightning(rng, x0, y0, x1, y1):
    ss = 2
    im = Image.new("L", (W * ss, H * ss), 0)
    dr = ImageDraw.Draw(im)
    step = px(5.0)
    segs = []

    def walk(x, y, tx, ty, width, bright, depth):
        pts = [(x, y)]
        dev = 0.0
        nsteps = max(2, int(math.hypot(tx - x, ty - y) / step * 1.12))
        for i in range(nsteps):
            base = math.atan2(ty - y, tx - x)
            dev = dev * 0.62 + rng.normal(0, 0.55)
            if rng.random() < 0.10:
                dev += rng.normal(0, 1.0)
            a = base + float(np.clip(dev, -1.35, 1.35))
            st = step * rng.uniform(0.6, 1.4)
            x += math.cos(a) * st
            y += math.sin(a) * st
            pts.append((x, y))
            rem = math.hypot(tx - x, ty - y)
            if rem < step * 1.2:
                break
            if depth < 3 and rng.random() < (0.22 if depth == 0 else 0.08):
                ba = a + rng.choice([-1.0, 1.0]) * rng.uniform(0.35, 0.95)
                bl = rem * rng.uniform(0.18, 0.55) if depth == 0 else rem * rng.uniform(0.3, 0.7)
                walk(x, y, x + math.cos(ba) * bl, y + math.sin(ba) * bl * 0.9 + bl * 0.25,
                     width * 0.55, bright * 0.7, depth + 1)
        if depth == 0:
            pts.append((tx, ty))
        segs.append((pts, width, bright))

    walk(x0, y0, x1, y1, px(3.0), 1.0, 0)
    for pts, width, bright in sorted(segs, key=lambda s: s[2]):
        m = len(pts)
        for i in range(m - 1):
            fade = 1.0 if bright >= 1.0 else 1.0 - 0.75 * i / m
            wd = max(1, int(round(width * ss * (1.0 - 0.6 * i / m))))   # tapers towards the tip
            dr.line([(pts[i][0] * ss, pts[i][1] * ss), (pts[i + 1][0] * ss, pts[i + 1][1] * ss)],
                    fill=int(255 * min(1.0, bright * fade)), width=wd)
    b = np.asarray(im, F32) / 255.0
    b = b.reshape(H, ss, W, ss).mean((1, 3))
    return gblur(b, px(0.8))


# --------------------------------------------------------------------------
# scene assembly
# --------------------------------------------------------------------------
def shade_terrain(G, amb, moon, moon_k, relief=1.0, snow_alb=0.86, flash=None):
    lit = G["diff"] * (1.0 - G["shadow"])
    near = smooth(520.0, 60.0, G["z"])
    lit = np.clip(lit + G["relief"] * 0.20 * relief * near, 0, 1.3)
    tex = 1.0 + 0.045 * G["grainy"] * near + 0.05 * G["rn"]
    rock = G["rock"][..., None]
    alb = snow_alb * (1 - rock) + rock * np.array([0.10, 0.105, 0.12], F32)
    ambf = (0.55 + 0.45 * G["ao"]) * tex * (1.0 + 0.22 * G["relief"] * relief * near)
    col = alb * (amb[None, None, :] * ambf[..., None] +
                 moon[None, None, :] * (lit * moon_k * tex)[..., None])
    if flash is not None:
        fcol, fmap = flash
        col = col + alb * fcol[None, None, :] * fmap[..., None]
    return col.astype(F32)


def apply_haze(col, G, haze_col, dens, hfall=0.0):
    """Distance fog; hfall>0 thins it with altitude (ground-hugging haze)."""
    f = 1.0 - np.exp(-(G["z"] / 1000.0) * dens * np.exp(-np.clip(G["h"], 0, None) * hfall))
    return col * (1 - f[..., None]) + haze_col * f[..., None]


def horizon_colour(sky):
    """Per-column haze colour sampled from the sky just above the horizon."""
    band = sky[HOR - ipx(40):HOR - ipx(6)].mean(0)
    half = ipx(80)
    k = np.ones(2 * half + 1, F32) / (2 * half + 1)
    band = np.stack([np.convolve(np.pad(band[:, i], half, mode="edge"), k, "valid")
                     for i in range(3)], -1)
    return band[None, :, :].astype(F32)


def finish(img, ev, rng, bloom=0.22, vign=0.30, grain=0.011, sat=1.0):
    hi = np.clip(img - 0.35, 0, None)
    img = img + gblur(hi, px(9.0)) * bloom + gblur(hi, px(30.0)) * bloom * 0.6
    small = img.reshape(H0, S, W0, S, 3).mean((1, 3))
    yy, xx = np.mgrid[0:H0, 0:W0].astype(F32)
    r2 = ((xx - W0 / 2) / (W0 * 0.62)) ** 2 + ((yy - H0 * 0.45) / (H0 * 0.95)) ** 2
    small = small * (1.0 - vign * np.clip(r2, 0, 1.2))[..., None]
    if sat != 1.0:
        l = small.mean(-1, keepdims=True)
        small = l + (small - l) * sat
    x = np.clip(small, 0, None)
    x = np.where(x < 0.6, x, 0.6 + 0.4 * np.tanh((x - 0.6) / 0.4))
    g = x ** (1 / 2.2)
    n = rng.standard_normal((H0, W0)).astype(F32)
    n = n * 0.6 + gblur(n, 0.8) * 1.3
    cn = rng.standard_normal((H0, W0, 3)).astype(F32) * 0.35
    g = g + grain * (n[..., None] + cn) * (0.55 + 0.6 * (1 - g))
    dither = rng.uniform(-0.5, 0.5, g.shape) / 255.0
    return (np.clip(g + dither, 0, 1) * 255.0 + 0.5).astype(np.uint8)


def storm_deck(seed, base_col, light_xy, gap_k, swirl=None, thr=-1.0, contrast=1.5, gap_h=0.12):
    """Heavy, fully covering cloud base with dark bellies and paler breaks.
    Returns (alpha, colour brightness field, raw noise, self-shadow light)."""
    dens, n = cloud_density(seed, thr, 1.6, 0.8, 0.8, 0.8, billow=0.30, gain=0.58, swirl=swirl)
    tex = smooth(-1.1, 1.4, n)
    light = cloud_light(tex, light_xy[0], light_xy[1], 8.0, 0.55)
    b = 0.22 + 2.3 * (1 - tex) ** contrast * light + 0.9 * light
    gap = smooth(gap_h * H, 0.1 * gap_h * H, HOR - YY)
    alpha = (dens * (1 - gap_k * gap))[..., None]
    return alpha, base_col[None, None, :] * b[..., None], tex, light


def cloud_ceiling(G, ter, sky, lo, hi, seed):
    """Peaks above the cloud base dissolve into the deck."""
    c = smooth(lo, hi, G["h"] + 7.0 * fbm(XX / W * 1.2, YY / W * 1.2, 4, 0.55, seed))[..., None]
    return ter * (1 - c) + sky * c


def scene(ev, G):
    global MOON_Y
    use_view(G)
    MOON_Y = MOON_H.get(ev, 0.20) * H
    rng = np.random.default_rng(5000 + ev)
    cov = G["cov"][..., None]
    moon_disc, moon_glow = moon_layer(rng)
    moon_col = srgb(232, 234, 224)
    post = {}

    if ev == 1:      # Clear Skies -------------------------------------------
        sky = sky_gradient(srgb(3, 6, 20), srgb(20, 46, 74), 0.6,
                           glow=(srgb(22, 60, 62), 0.42, 0.5, 0.10), seed=1)
        sky += stars_layer(rng, 1900)
        sky += aurora_layer(rng, [(0.33, -0.20, 1.2, 0.055, 0.30, 3),
                                  (0.25, 0.12, 1.4, 0.045, 0.15, 9),
                                  (0.52, -0.06, 3.0, 0.030, 0.09, 15)]) * \
            smooth(0.0, 0.1 * H, HOR - YY)[..., None]
        sky = sky * (1 - moon_disc[..., None]) + moon_col * 1.15 * moon_disc[..., None]
        sky += moon_glow[..., None] * srgb(150, 175, 215)
        amb = srgb(26, 50, 78) + srgb(4, 30, 22)
        ter = shade_terrain(G, amb, srgb(170, 190, 225), 1.25)
        ter = apply_haze(ter, G, horizon_colour(sky), 1.0, 0.012)
        img = sky * (1 - cov) + ter * cov

    elif ev == 2:    # Light Cloud Coverage ----------------------------------
        sky = sky_gradient(srgb(7, 12, 32), srgb(34, 54, 90), 0.6, seed=2)
        sky += stars_layer(rng, 1200) * 0.8
        sky = sky * (1 - moon_disc[..., None]) + moon_col * 1.15 * moon_disc[..., None]
        dens, _ = cloud_density(210, 0.25, 1.1, 1.0, 1.0, 0.6, gain=0.56)
        light = cloud_light(dens, MOON_X, MOON_Y, 7.0, 0.8)
        prox = np.exp(-np.hypot(XX - MOON_X, YY - MOON_Y) / (0.22 * W))
        ccol = srgb(18, 26, 46)[None, None, :] * (1.0 - 0.45 * dens[..., None]) + \
            srgb(150, 168, 205)[None, None, :] * (light * (0.26 + 1.7 * prox))[..., None]
        alpha = (1 - np.exp(-dens * 3.0))[..., None]
        sky = sky * (1 - alpha) + ccol * alpha
        sky += (moon_glow * (1 + 1.2 * dens))[..., None] * srgb(160, 180, 215)
        ter = shade_terrain(G, srgb(30, 44, 72), srgb(160, 178, 212), 0.95)
        ter = apply_haze(ter, G, horizon_colour(sky), 1.2, 0.012)
        img = sky * (1 - cov) + ter * cov

    elif ev == 3:    # Overcast ----------------------------------------------
        sky = sky_gradient(srgb(40, 46, 56), srgb(70, 78, 90), 0.8, seed=3)
        dens, n = cloud_density(310, -1.6, 2.6, 0.9, 0.6, 0.6, gain=0.55, billow=0.2)
        light = cloud_light(smooth(-1.0, 1.2, n), MOON_X, MOON_Y - 0.4 * H, 8.0, 0.5)
        prox = np.exp(-np.hypot(XX - MOON_X, (YY - MOON_Y) * 1.5) / (0.26 * W))
        b = 0.30 + 1.25 * smooth(1.3, -1.3, n) * light + 0.25 * light + 0.75 * prox
        hz = smooth(0.26 * H, 0.0, HOR - YY)
        b = b * (1 - 0.35 * hz) + 0.35 * hz * 0.95
        sky = sky * 0.12 + srgb(96, 104, 118)[None, None, :] * b[..., None] * 0.88
        ter = shade_terrain(G, srgb(72, 80, 94), srgb(96, 104, 118), 0.28, relief=0.6)
        ter = apply_haze(ter, G, horizon_colour(sky), 1.3, 0.0)
        ter = cloud_ceiling(G, ter, sky, 40.0, 62.0, 312)
        img = sky * (1 - cov) + ter * cov
        post = dict(sat=0.85, bloom=0.1)

    elif ev == 4:    # Storm Clouds ------------------------------------------
        hcol = srgb(126, 138, 152)
        sky = sky_gradient(srgb(8, 10, 18), hcol, 1.5, seed=4)
        # heavy sculpted base, lit from the strip of clear sky left at the horizon
        alpha, ccol, tex, light = storm_deck(410, srgb(58, 66, 86), (0.42 * W, HOR + 0.02 * H), 0.97,
                                             contrast=2.2, gap_h=0.36)
        under = smooth(0.62 * H, 0.0, HOR - YY) ** 1.6
        ccol = ccol * (0.55 + 0.45 * under[..., None]) + \
            hcol[None, None, :] * (under * (0.15 + 0.85 * light) * (1.25 - tex) * 0.85)[..., None]
        sky = sky * (1 - alpha) + ccol * alpha
        ter = shade_terrain(G, srgb(44, 54, 74), srgb(104, 116, 136), 0.36, relief=0.8)
        ter = apply_haze(ter, G, horizon_colour(sky) * 0.55, 0.8, 0.0)
        ter = cloud_ceiling(G, ter, sky, 66.0, 86.0, 412)
        img = sky * (1 - cov) + ter * cov
        post = dict(vign=0.30, bloom=0.14)

    elif ev == 5:    # Light Snow Dusting ------------------------------------
        sky = sky_gradient(srgb(22, 30, 54), srgb(62, 78, 108), 0.7, seed=5)
        sky += stars_layer(rng, 500) * 0.25
        sky = sky * (1 - 0.8 * moon_disc[..., None]) + moon_col * 0.8 * moon_disc[..., None]
        dens, _ = cloud_density(510, -1.2, 3.0, 0.8, 0.6, 0.5, gain=0.5)
        light = cloud_light(dens, MOON_X, MOON_Y, 8.0, 0.6)
        prox = np.exp(-np.hypot(XX - MOON_X, YY - MOON_Y) / (0.25 * W))
        ccol = srgb(44, 56, 84)[None, None, :] * (0.8 + 0.5 * light[..., None]) + \
            srgb(150, 165, 195)[None, None, :] * (light * prox * 1.2)[..., None]
        alpha = (1 - np.exp(-dens * 1.1))[..., None]
        sky = sky * (1 - alpha) + ccol * alpha
        sky += (moon_glow * 1.6)[..., None] * srgb(150, 168, 200)
        ter = shade_terrain(G, srgb(48, 62, 90), srgb(140, 156, 190), 0.55)
        ter = apply_haze(ter, G, horizon_colour(sky), 1.5, 0.0)
        img = sky * (1 - cov) + ter * cov
        white = srgb(225, 232, 245)
        img = over(img, snow(rng, 2200, 0.40, 0.75, 3, 82), white)
        img = over(img, snow(rng, 520, 0.60, 1.0, 5, 80), white)
        img = over(img, snow(rng, 110, 0.55, 1.6, 8, 78, calm=True), white)
        img = over(img, snow(rng, 22, 0.22, 3.6, 10, 78, calm=True), white)

    elif ev == 6:    # Heavy Snowfall ----------------------------------------
        sky = sky_gradient(srgb(50, 58, 76), srgb(94, 104, 124), 0.8, seed=6)
        tex = fbm(XX / W * 0.7, YY / W * 1.1, 5, 0.55, 610)
        sky = sky * (1 + 0.14 * tex[..., None])
        ter = shade_terrain(G, srgb(84, 94, 114), srgb(120, 130, 150), 0.25, relief=0.6)
        ter = apply_haze(ter, G, horizon_colour(sky), 2.0, 0.0)
        img = sky * (1 - cov) + ter * cov
        # falling-snow veils in the distance
        veil = fbm(XX / W * 1.2 + YY / W * 0.25, YY / W * 0.12, 5, 0.6, 611)
        img = over(img, 0.30 * smooth(-0.6, 1.2, veil), srgb(118, 128, 148))
        white = srgb(232, 238, 248)
        img = over(img, snow(rng, 9000, 0.30, 0.70, 5, 76), white)
        img = over(img, snow(rng, 3000, 0.50, 0.95, 8, 74), white)
        img = over(img, snow(rng, 650, 0.62, 1.4, 12, 72), white)
        img = over(img, snow(rng, 80, 0.40, 2.4, 18, 70, calm=True), white)
        img = over(img, snow(rng, 26, 0.22, 4.6, 22, 70, calm=True), white)
        post = dict(sat=0.9, bloom=0.1, vign=0.25)

    elif ev == 7:    # Blizzard ----------------------------------------------
        # the range stays readable as a silhouette behind the driving snow
        sky = sky_gradient(srgb(78, 90, 112), srgb(118, 130, 150), 0.8, seed=7)
        ter = shade_terrain(G, srgb(34, 42, 62), srgb(120, 132, 150), 0.34, relief=0.9)
        ter = ter * (1.0 - 0.45 * smooth(150.0, 420.0, G["z"]))[..., None]
        ter = apply_haze(ter, G, horizon_colour(sky), 0.75, 0.0)
        img = sky * (1 - cov) + ter * cov
        ang = 14.0
        sh = wind_sheets(710, ang, 5.0, 1.3)
        sh2 = wind_sheets(720, ang + 3, 9.0, 4.0)
        img = over(img, 0.24 * smooth(-1.0, 1.6, sh) + 0.12 * smooth(-0.5, 1.5, sh2),
                   srgb(150, 162, 182))
        ground = smooth(HOR - 0.02 * H, HOR + 0.10 * H, YY)
        drift = wind_sheets(730, 4.0, 10.0, 6.0)
        img = over(img, 0.5 * ground * smooth(-0.3, 1.6, drift), srgb(186, 196, 212))
        white = srgb(236, 242, 250)
        s1 = snow(rng, 2600, 0.17, 0.75, 26, ang)
        s2 = snow(rng, 700, 0.36, 1.1, 48, ang + 2)
        s3 = snow(rng, 110, 0.40, 2.2, 90, ang + 4, calm=True)
        s = warp(np.maximum(np.maximum(s1, s2), s3), 10, 34, 0.30, 740)
        img = over(img, s, white)
        post = dict(sat=0.9, bloom=0.08, vign=0.30)

    elif ev == 8:    # White Out ---------------------------------------------
        # a featureless white void: no streaks, only the faintest hint of the
        # drifts at your feet before everything dissolves
        sky = sky_gradient(srgb(196, 203, 214), srgb(214, 219, 226), 0.9, seed=8)
        ter = shade_terrain(G, srgb(168, 178, 194), srgb(120, 130, 150), 0.30, relief=1.6)
        f = 1.0 - np.exp(-np.clip(G["z"] - 3.0, 0, None) / 26.0)
        ter = ter * (1 - f[..., None]) + horizon_colour(sky) * f[..., None]
        img = sky * (1 - cov) + ter * cov
        sh = wind_sheets(810, 6.0, 3.0, 0.8, octaves=4, gain=0.5)
        sh2 = wind_sheets(820, 8.0, 6.0, 2.2, octaves=4, gain=0.5)
        img = img * (1.0 + 0.045 * sh[..., None] + 0.02 * sh2[..., None])
        img = over(img, 0.30 * smooth(-0.8, 1.4, sh), srgb(232, 236, 242))
        # spindrift skimming the ground, then a few soft flakes right at the lens
        low = smooth(HOR + 0.02 * H, H * 0.98, YY)
        img = over(img, 0.35 * low * smooth(-0.4, 1.4, wind_sheets(830, 3.0, 9.0, 5.0)), srgb(240, 243, 248))
        white = srgb(252, 253, 255)
        img = over(img, 0.5 * warp(snow(rng, 1500, 0.22, 1.3, 20, 8.0), 6, 18, 0.30, 840), white)
        img = over(img, snow(rng, 40, 0.16, 5.0, 26, 9.0), white)
        post = dict(sat=0.8, bloom=0.0, vign=0.26, grain=0.009)

    elif ev == 9:    # Extreme Fog -------------------------------------------
        sky = sky_gradient(srgb(40, 52, 56), srgb(74, 90, 90), 0.9, seed=9)
        ter = shade_terrain(G, srgb(22, 30, 34), srgb(90, 106, 110), 0.34, relief=1.2)
        fogc = srgb(86, 104, 102)
        d = np.hypot(XX - MOON_X, (YY - MOON_Y) * 1.3)
        glowf = 0.60 * np.exp(-d / (0.14 * W)) + 0.38 * np.exp(-d / (0.45 * W))
        bank2 = fbm(XX / W * 0.35 + 2.0, YY / W * 0.9, 5, 0.55, 911)
        fogcol = fogc[None, None, :] * ((0.70 + 1.3 * glowf) * (1.0 + 0.10 * bank2))[..., None]
        bank = fbm(XX / W * 0.55, YY / W * 2.4, 6, 0.58, 910)
        f = 1.0 - np.exp(-((G["z"] - 10.0) / 1000.0) * 3.4 * (1.0 + 0.22 * bank))
        ter = ter * (1 - f[..., None]) + fogcol * f[..., None]
        img = fogcol * (1 - cov) + ter * cov
        low = smooth(HOR - 0.20 * H, HOR + 0.04 * H, YY) * smooth(H * 0.98, HOR + 0.08 * H, YY)
        img = over(img, 0.45 * low * smooth(-0.9, 1.0, bank), fogc * 1.25)
        post = dict(sat=0.95, bloom=0.05, vign=0.45)

    elif ev == 10:   # Lightning Storm ---------------------------------------
        sky = sky_gradient(srgb(8, 6, 20), srgb(50, 44, 82), 0.4, seed=10)
        bx0, by0 = 0.56 * W, -0.04 * H          # the channel starts up inside the cloud
        bx1 = 0.50 * W
        by1 = float(G["skyline"][int(bx1)]) + px(6.0)
        alpha, ccol, tex, light = storm_deck(1010, srgb(34, 30, 56), (bx0, 0.16 * H), 0.6)
        bolt = lightning(rng, bx0, by0, bx1, by1)
        # the cloud base swallows the top of the channel
        bolt = bolt * (0.25 + 0.75 * smooth(0.02 * H, 0.16 * H, YY))
        halo = gblur(bolt, px(30.0)) * 22.0 + gblur(bolt, px(100.0)) * 70.0
        src = np.exp(-np.hypot(XX - bx0, (YY - 0.04 * H) * 1.6) / (0.17 * W))
        flash = np.clip(halo + 2.2 * src, 0, 3.5)
        fcol = srgb(200, 205, 255)              # lit areas go blue-white, shadows stay violet
        ccol = ccol + fcol[None, None, :] * (flash * (0.10 + 1.2 * light * (1.15 - tex)))[..., None]
        sky = sky * (1 - alpha) + ccol * alpha
        gf = np.exp(-np.hypot((XX - bx1) / (0.42 * W), (YY - by1) / (0.50 * H)))
        ter = shade_terrain(G, srgb(22, 20, 40), srgb(150, 140, 200), 0.30, relief=1.0,
                            flash=(srgb(226, 230, 255), 0.08 + 0.80 * gf * (0.30 + 0.70 * G["ao"])))
        ter = apply_haze(ter, G, horizon_colour(sky), 1.2, 0.0)
        ter = cloud_ceiling(G, ter, sky, 50.0, 70.0, 1012)
        img = sky * (1 - cov) + ter * cov
        # strike point: a hot glow where the channel meets the ground
        gd = np.hypot(XX - bx1, (YY - by1) * 2.2)
        img += (0.9 * np.exp(-gd / (0.012 * W)) + 0.22 * np.exp(-gd / (0.06 * W)))[..., None] * srgb(215, 220, 255)
        img += (gblur(bolt, px(1.5)) * 0.9 + gblur(bolt, px(5.0)) * 1.0 + gblur(bolt, px(16.0)) * 1.5)[..., None] * \
            srgb(170, 175, 255)
        img += (bolt * 6.0)[..., None] * srgb(250, 250, 255)
        post = dict(bloom=0.28, vign=0.42)

    elif ev == 11:   # Hail Storm --------------------------------------------
        sky = sky_gradient(srgb(12, 16, 24), srgb(96, 108, 122), 0.40, seed=11)
        alpha, ccol, tex, light = storm_deck(1110, srgb(34, 40, 52), (0.4 * W, HOR + 0.3 * H), 0.85,
                                             thr=-1.1, contrast=1.8, gap_h=0.30)
        sky = sky * (1 - alpha) + ccol * alpha
        ter = shade_terrain(G, srgb(44, 54, 68), srgb(104, 116, 132), 0.38, relief=0.9)
        # fallen hailstones: a pebbled crust plus discrete stones lying on the snow
        peb = fbm(G["mx"] / 3.0, G["my"] / 3.0, 3, 0.8, 1111, fp=G["fp"] / 3.0)
        ter = ter * (1.0 + 0.6 * smooth(0.5, 1.8, peb) * smooth(260.0, 30.0, G["z"]))[..., None]
        ter = apply_haze(ter, G, horizon_colour(sky) * 0.7, 1.2, 0.0)
        ter = cloud_ceiling(G, ter, sky, 58.0, 72.0, 1112)
        img = sky * (1 - cov) + ter * cov
        white = srgb(238, 243, 250)
        ice = srgb(206, 220, 236)
        nst = 520
        gy = HOR + px(6) + (H - HOR - px(6)) * rng.uniform(0, 1, nst) ** 1.5
        gx = rng.uniform(0, W, nst)
        gs = (gy - HOR) / (H - HOR)
        far, near = gs < 0.45, gs >= 0.45
        st_f = np.zeros((H, W), F32)
        st_n = np.zeros((H, W), F32)
        splat(st_f, gx[far], gy[far], rng.uniform(0.3, 1.0, far.sum()) * 0.40)
        splat(st_n, gx[near], gy[near], rng.uniform(0.3, 1.0, near.sum()) * 0.8)
        stones = streak_filter(st_f, px(0.9), 0, 0) + streak_filter(st_n, px(1.7), 0, 0)
        img = img * (1.0 - 0.35 * np.clip(np.roll(stones, (ipx(2), -ipx(2)), (0, 1)), 0, 1) * calm_mask())[..., None]
        img = over(img, 0.85 * stones * calm_mask(), ice)
        # falling stones: round pellets with a short tail, not streaks
        ang = 78.0
        img = over(img, snow(rng, 2400, 0.26, 0.70, 9, ang), ice)
        for cnt, amp, sig, ln, calm in ((420, 0.70, 1.15, 13, False), (90, 0.95, 1.9, 17, True),
                                        (12, 0.60, 3.4, 22, True)):
            pos = np.zeros((H, W), F32)
            qx, qy = rng.uniform(0, W, cnt), rng.uniform(0, H, cnt)
            pa = rng.uniform(0.35, 1.0, cnt) * amp
            splat(pos, qx, qy, pa)
            tail = streak_filter(pos, px(sig * 0.7), px(ln), ang)          # faint motion tail
            head = np.zeros((H, W), F32)                           # hard bead at the leading end
            a_r = math.radians(ang)
            splat(head, qx + math.cos(a_r) * px(ln) * 0.5, qy + math.sin(a_r) * px(ln) * 0.5, pa)
            bead = np.clip(streak_filter(head, px(sig), 0, 0), 0, None)
            lay = np.clip(0.26 * tail, 0, None) + bead
            if calm:
                lay = lay * calm_mask()
            img = over(img, lay, white)
        # impact bursts where stones hit the ground
        sp = np.zeros((H, W), F32)
        nimp = 160
        sy = HOR + px(8) + (H - HOR - px(8)) * rng.uniform(0, 1, nimp) ** 1.6
        sx = rng.uniform(0, W, nimp)
        sc = (sy - HOR) / (H - HOR)
        for j in range(7):
            r = rng.uniform(0.3, 1.0, nimp) * px(1.5 + 8.0 * sc)
            a = rng.uniform(-1.2, 1.2, nimp)
            splat(sp, sx + np.sin(a) * r, sy - np.cos(a) * r * 0.8,
                  rng.uniform(0.3, 1.0, nimp) * (0.4 + 0.6 * sc))
        sp = (gblur(sp, px(0.9)) * 0.75 + gblur(sp, px(4.5)) * 2.0) * K * K * calm_mask()
        img = over(img, sp, white)
        post = dict(bloom=0.15, vign=0.36, sat=0.9)

    elif ev == 12:   # Wind Vortex -------------------------------------------
        cx0, yb = 0.50 * W, HOR + 0.045 * H
        ytop = -0.10 * H
        sky = sky_gradient(srgb(12, 12, 30), srgb(84, 60, 84), 0.5,
                           glow=(srgb(120, 72, 66), 0.50, 0.45, 0.06), seed=12)
        sky += stars_layer(rng, 500) * 0.3
        alpha, ccol, tex, light = storm_deck(1210, srgb(42, 36, 60), (0.5 * W, HOR + 0.2 * H), 0.88,
                                             swirl=(cx0 + 0.02 * W, 0.02 * H, 2.4, 0.20 * W), thr=-0.6)
        ccol = ccol + srgb(120, 70, 70)[None, None, :] * \
            (light * smooth(0.45 * H, 0.0, HOR - YY) * 0.5)[..., None]
        sky = sky * (1 - alpha) + ccol * alpha
        ter = shade_terrain(G, srgb(40, 40, 68), srgb(140, 120, 130), 0.50)
        ter = apply_haze(ter, G, horizon_colour(sky) * 0.8, 1.1, 0.008)
        ter = cloud_ceiling(G, ter, sky, 54.0, 74.0, 1212)
        img = sky * (1 - cov) + ter * cov
        # the funnel
        hraw = (yb - YY) / (yb - ytop)
        hgt = np.clip(hraw, 0, 1.2)
        cx = cx0 + 0.030 * W * np.sin(hgt * 3.3 + 0.6) * hgt + 0.012 * W * np.sin(hgt * 9.0 + 2.0) * hgt
        rad = px(8.0) + 0.085 * W * hgt ** 1.7 + px(6.0) * hgt
        edge = fbm(XX / W * 2.2, YY / W * 3.5, 5, 0.6, 1213)
        dxn = (XX - cx) / rad + 0.14 * edge
        ins = np.clip(1.0 - dxn * dxn, 0, 1)
        shell = np.sqrt(ins) - 0.80 * np.sqrt(np.clip(0.70 - dxn * dxn, 0, 1))
        th = np.arcsin(np.clip(dxn, -1, 1))
        front = fbm(th * 0.20 + hgt * 0.9, hgt * 0.55 - th * 0.05, 6, 0.62, 1214)
        back = fbm(-th * 0.20 + hgt * 0.9 + 3.0, hgt * 0.55 + th * 0.05, 5, 0.62, 1215)
        fd = shell * (0.62 + 0.42 * front + 0.26 * back)
        fd = fd * smooth(1.25, 0.55, hgt) * smooth(0.0, 0.035, hraw)
        fd = gblur(np.clip(fd, 0, None), px(2.0))
        lit = 0.84 + 0.30 * np.clip(dxn, -1, 1) + 0.16 * front
        pale = srgb(186, 182, 200)
        vcol = pale[None, None, :] * (lit * (0.62 + 0.38 * smooth(1.0, 0.15, hgt)))[..., None]
        a = np.clip(fd * 1.25, 0, 0.94)[..., None]
        img = img * (1 - a) + vcol * a
        # spindrift skirt and blown snow racing in along the ground
        dxb = (XX - cx0) / (0.11 * W)
        up = np.clip(yb - YY, 0, None)
        skirt = np.exp(-dxb * dxb) * np.exp(-up / (0.05 * H)) * smooth(yb + 0.05 * H, yb - 0.005 * H, YY)
        sk_n = fbm(XX / W * 1.8, YY / W * 5.0, 5, 0.6, 1216)
        img = over(img, 0.85 * skirt * (0.75 + 0.35 * sk_n), pale * 0.85)
        gr = smooth(HOR - 0.01 * H, HOR + 0.06 * H, YY) * smooth(H * 1.0, HOR + 0.12 * H, YY)
        sheets = wind_sheets(1217, 2.0, 12.0, 7.0)
        img = over(img, 0.40 * gr * smooth(-0.2, 1.5, sheets) * calm_mask(), pale * 0.8)
        s = warp(snow(rng, 2600, 0.20, 0.9, 44, 5.0), 10, 24, 0.35, 1218)
        img = over(img, s * smooth(HOR - 0.12 * H, HOR + 0.22 * H, YY), srgb(215, 208, 220))
        post = dict(bloom=0.14, vign=0.40)

    return finish(img, ev, rng, **post)


def save_jpeg(arr, path):
    im = Image.fromarray(arr, "RGB")
    for q in range(92, 40, -2):
        buf = io.BytesIO()
        im.save(buf, "JPEG", quality=q, subsampling=0, optimize=True, progressive=False)
        if buf.tell() <= MAX_BYTES:
            break
    with open(path, "wb") as f:
        f.write(buf.getvalue())
    return buf.tell(), q


def render_view(job):
    """Build one terrain view and render every requested scene that uses it."""
    name, evs = job
    G = build_gbuffer(VIEWS[name])
    out = []
    for ev in evs:
        size, q = save_jpeg(scene(ev, G), os.path.join(OUT_DIR, "w%02d.jpg" % ev))
        out.append((ev, size, q))
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--width")]
    if "--width" in sys.argv:
        args.remove(sys.argv[sys.argv.index("--width") + 1])
    ids = [int(a) for a in args] or list(range(1, 13))
    os.makedirs(OUT_DIR, exist_ok=True)
    jobs = {}
    for ev in ids:
        jobs.setdefault(EVENT_VIEW[ev], []).append(ev)
    jobs = sorted(jobs.items())
    # the four views are independent; every random stream is seeded, so the
    # output is identical whether they run in parallel or one after another
    nproc = min(len(jobs), os.cpu_count() or 1)
    if nproc > 1:
        import multiprocessing as mp
        with mp.get_context("fork").Pool(nproc) as pool:
            results = pool.map(render_view, jobs, chunksize=1)
    else:
        results = [render_view(j) for j in jobs]
    total = 0
    for ev, size, q in sorted(r for res in results for r in res):
        total += size
        print("w%02d.jpg  %6d bytes  q=%d" % (ev, size, q))
    print("total    %6d bytes" % total)


if __name__ == "__main__":
    main()
