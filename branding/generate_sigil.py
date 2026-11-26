#!/usr/bin/env python3
"""Afterglow sigil: draws afterglow-sigil.svg through VectorCraft's control port.

Eight longswords hung inside a heavy iron seal ring, points turned in, biting a
rotten blood moon. Fixed seeds and palette, so every run gives the same SVG.

    nix-shell -p vectorcraft --run "vectorcraft --control 7979" &
    python3 generate_sigil.py [out_dir]
"""
import json, math, os, random, socket, sys

HOST, PORT = "127.0.0.1", int(os.environ.get("VCPORT", "7979"))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))

# --------------------------------------------------------------------- palette
FIELD    = "#060608"
IRON     = "#14161b"
IRON_HI  = "#242833"
STEEL    = "#8e939b"
STEEL_D  = "#5d626b"
STEEL_HI = "#d3d7dd"
ENGRAVE  = "#6a707a"
ENGRAVE_D= "#3a3f48"
BLOOD    = "#5f0b0a"
BLOOD_HI = "#7c1a14"
BLOOD_D  = "#2e0403"
BLOOD_IN = "#380605"
MOON     = "#260608"
MOON_RIM = "#6a1712"
MOON_ENG = "#45100c"

CX = CY = 512.0
R_FIELD   = 360.0
R_BAND    = 404.0
R_GUARD   = 318.0
BLADE_L   = 192.0
BLADE_W   = 13.0
MOON_R    = 186.0
N         = 8

# ----------------------------------------------------------------- transport
def send(obj):
    s = socket.create_connection((HOST, PORT), timeout=180)
    f = s.makefile("rwb")
    f.write((json.dumps(obj) + "\n").encode()); f.flush()
    line = f.readline(); s.close()
    return json.loads(line)

def cmd(command, **params):
    return send({"method": "command", "params": {"command": command, "params": params}})

def ok(res):
    if not res.get("ok"):
        raise RuntimeError(res.get("error", res))
    return res["result"]

def oid(res):
    return ok(res)["id"]

# ------------------------------------------------------------------ painting
def poly(pts):
    d = "M " + " L ".join("%.2f %.2f" % (x, y) for x, y in pts) + " Z"
    return oid(cmd("path.create", d=d))

def shape(pts, fill=None, stroke=None, w=2.0, cap="round"):
    i = poly(pts)
    ok(cmd("paint.setFill", ids=[i], color=fill) if fill else cmd("paint.setFill", ids=[i], none=True))
    if stroke:
        ok(cmd("paint.setStroke", ids=[i], color=stroke))
        ok(cmd("stroke.set", ids=[i], weight=w, cap=cap, join="round"))
    else:
        ok(cmd("paint.setStroke", ids=[i], none=True))
    return i

def stroke_path(pts, c, w, cap="round"):
    """Open stroke, no fill."""
    d = "M " + " L ".join("%.2f %.2f" % (x, y) for x, y in pts)
    i = oid(cmd("path.create", d=d))
    ok(cmd("paint.setFill", ids=[i], none=True))
    ok(cmd("paint.setStroke", ids=[i], color=c))
    ok(cmd("stroke.set", ids=[i], weight=w, cap=cap, join="round"))
    return i

def disc(cx, cy, r, c):
    return shape_ellipse(cx, cy, r, r, c)

def shape_ellipse(cx, cy, rx, ry, c):
    i = oid(cmd("shape.ellipse", x=cx - rx, y=cy - ry, width=2 * rx, height=2 * ry))
    if c:
        ok(cmd("paint.setFill", ids=[i], color=c))
    else:
        ok(cmd("paint.setFill", ids=[i], none=True))
    ok(cmd("paint.setStroke", ids=[i], none=True))
    return i

def blur(i, r):
    ok(cmd("effect.apply", ids=[i], effect="blur.gaussian", params={"radius": r})); return i

# ----------------------------------------------------------------- randomness
import random
rnd = random.Random(4021)
RS = random.Random(31)

def jit(v, d):
    return v + rnd.uniform(-d, d)

def arc(r, a0, a1, n=64):
    return [(CX + r * math.cos(math.radians(a0 + (a1 - a0) * k / n)),
             CY + r * math.sin(math.radians(a0 + (a1 - a0) * k / n))) for k in range(n + 1)]

# ---------------------------------------------------------------- primitives
def smooth(pts, steps=18):
    """Catmull-Rom through the control points: a clean centreline for ribbons."""
    P = [pts[0]] + list(pts) + [pts[-1]]
    out = []
    for i in range(len(P) - 3):
        p0, p1, p2, p3 = P[i], P[i + 1], P[i + 2], P[i + 3]
        for k in range(steps):
            t = k / steps
            t2, t3 = t * t, t * t * t
            x = 0.5 * (2 * p1[0] + (-p0[0] + p2[0]) * t +
                       (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2 +
                       (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3)
            y = 0.5 * (2 * p1[1] + (-p0[1] + p2[1]) * t +
                       (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2 +
                       (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3)
            out.append((x, y))
    out.append(pts[-1])
    return out

def ribbon(pts, half, taper=None, c=None, stroke=None, sw=1.4):
    """Turn a centreline into a closed tapered band (ink stroke that varies)."""
    left, right = [], []
    n = len(pts)
    for i, (x, y) in enumerate(pts):
        x0, y0 = pts[max(0, i - 1)]; x1, y1 = pts[min(n - 1, i + 1)]
        dx, dy = x1 - x0, y1 - y0
        L = math.hypot(dx, dy) or 1.0
        ux, uy = -dy / L, dx / L
        t = i / max(1, n - 1)
        w = half * (taper(t) if taper else 1.0)
        left.append((x + ux * w, y + uy * w))
        right.append((x - ux * w, y - uy * w))
    return shape(left + right[::-1], fill=c, stroke=stroke, w=sw)

def drip(x, y, length, w0, c=BLOOD, c2=None, seed=None):
    """A hanging bead of blood: rounded head, tapering tail."""
    pts = [(x, y)]
    n = 10
    for k in range(1, n + 1):
        t = k / n
        pts.append((x + math.sin(t * 3.1 + (seed or 0)) * 1.6, y - length * t))
    ribbon(pts, w0, taper=lambda t: (1 - t) ** 0.85 + 0.06, c=c)
    disc(x, y, w0 * 1.05, c)
    if c2:
        disc(x - w0 * 0.3, y - w0 * 0.25, w0 * 0.42, c2)

def fleck(x, y, r, c=BLOOD_D):
    m = rnd.randint(4, 7)
    pts = []
    for k in range(m):
        a = 2 * math.pi * k / m + jit(0, 0.4)
        rr = r * jit(1.0, 0.45)
        pts.append((x + rr * math.cos(a), y + rr * math.sin(a)))
    shape(pts, fill=c)

def splatter(x, y, spread, n, rmax):
    for _ in range(n):
        a = rnd.uniform(0, 2 * math.pi)
        d = spread * rnd.uniform(0.25, 1.0) ** 0.6
        fleck(x + d * math.cos(a), y + d * math.sin(a) * 0.9, rmax * rnd.uniform(0.25, 1.0))

# ---------------------------------------------------------------------- drawing
class Path:
    """Bezier path builder: every point goes through a transform first."""
    def __init__(self, T):
        self.T = T
        self.segs = []

    def _p(self, x, y):
        X, Y = self.T(x, y)
        return "%.2f %.2f" % (X, Y)

    def M(self, x, y):
        self.segs.append("M " + self._p(x, y)); return self

    def L(self, x, y):
        self.segs.append("L " + self._p(x, y)); return self

    def Q(self, x1, y1, x2, y2):
        self.segs.append("Q " + self._p(x1, y1) + " " + self._p(x2, y2))
        return self

    def C(self, x1, y1, x2, y2, x3, y3):
        self.segs.append("C " + self._p(x1, y1) + " " + self._p(x2, y2) + " " + self._p(x3, y3))
        return self

    def Z(self):
        self.segs.append("Z"); return self

    def d(self):
        return " ".join(self.segs)

def draw(d, fill=None, stroke=None, w=1.0, join="miter", cap="butt"):
    i = oid(cmd("path.create", d=d))
    ok(cmd("paint.setFill", ids=[i], color=fill) if fill else cmd("paint.setFill", ids=[i], none=True))
    if stroke:
        ok(cmd("paint.setStroke", ids=[i], color=stroke))
        ok(cmd("stroke.set", ids=[i], weight=w, cap=cap, join=join))
    else:
        ok(cmd("paint.setStroke", ids=[i], none=True))
    return i

def ell(cx, cy, r, fill=None, stroke=None, w=1.0, ry=None):
    res = ok(cmd("shape.ellipse", x=cx - r, y=cy - (ry or r), width=2 * r, height=2 * (ry or r)))
    i = res["id"]
    ok(cmd("paint.setFill", ids=[i], color=fill) if fill else cmd("paint.setFill", ids=[i], none=True))
    if stroke:
        ok(cmd("paint.setStroke", ids=[i], color=stroke))
        ok(cmd("stroke.set", ids=[i], weight=w, cap="round", join="round"))
    else:
        ok(cmd("paint.setStroke", ids=[i], none=True))
    return i

def arc_pts(cx, cy, r, a0, a1, n=96):
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * k / n)),
             cy + r * math.sin(math.radians(a0 + (a1 - a0) * k / n))) for k in range(n + 1)]

def arc_d(cx, cy, r, a0, a1):
    """A circular arc as a chain of quarter-ish cubics (max 20 degrees each)."""
    P = Path(lambda x, y: (x, y))
    steps = max(1, int(abs(a1 - a0) / 20.0) + 1)
    da = (a1 - a0) / steps
    k = 4.0 / 3.0 * math.tan(math.radians(da) / 4.0)
    a = a0
    P.M(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a)))
    for _ in range(steps):
        b = a + da
        x0, y0 = cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a))
        x1, y1 = cx + r * math.cos(math.radians(b)), cy + r * math.sin(math.radians(b))
        P.C(x0 - k * r * math.sin(math.radians(a)), y0 + k * r * math.cos(math.radians(a)),
            x1 + k * r * math.sin(math.radians(b)), y1 - k * r * math.cos(math.radians(b)),
            x1, y1)
        a = b
    return P.d()

def circle_d(cx, cy, r):
    return arc_d(cx, cy, r, 0, 360) + " Z"
# ----------------------------------------------------------------------- parts
def field():
    """Transparent ground: nothing is painted behind the mark."""

def band():
    # the band is an annulus: outer circle minus the inner one (opposite winding)
    ring_d = circle_d(CX, CY, R_BAND) + " " + arc_d(CX, CY, R_FIELD, 360, 0) + " Z"
    ri = oid(cmd("path.create", d=ring_d))
    ok(cmd("paint.setFill", ids=[ri], color=IRON))
    ok(cmd("paint.setStroke", ids=[ri], none=True))
    for r, w in ((399.0, 1.5), (392.0, 0.8), (366.0, 1.3), (358.0, 0.8)):
        draw(circle_d(CX, CY, r), stroke=ENGRAVE, w=w)
    # chevrons cut into the band
    for k in range(24):
        a = math.radians(15.0 * k + 7.5)
        ux, uy = math.cos(a), math.sin(a)
        rx, ry = -uy, ux
        def q(r, s):
            return (CX + ux * r + rx * s, CY + uy * r + ry * s)
        P = Path(lambda x, y: (x, y))
        p0, p1, p2 = q(376, -6), q(384, 0), q(376, 6)
        P.M(*p0).L(*p1).L(*p2)
        draw(P.d(), stroke=ENGRAVE, w=1.3, join="miter")
    # fine radial hatching inside the band
    for k in range(96):
        a = math.radians(3.75 * k)
        r1 = 370 + (6 if k % 3 == 0 else 0)
        r2 = 396 - (4 if k % 4 == 0 else 0)
        P = Path(lambda x, y: (x, y))
        P.M(CX + r1 * math.cos(a), CY + r1 * math.sin(a)).L(CX + r2 * math.cos(a), CY + r2 * math.sin(a))
        draw(P.d(), stroke=IRON_HI, w=0.8, cap="butt")

    # studs between the swords
    for k in range(N):
        a = math.radians(-90 + 45 * k + 22.5)
        x, y = CX + 380 * math.cos(a), CY + 380 * math.sin(a)
        ell(x, y, 6.4, fill=ENGRAVE, stroke=IRON, w=1.2)
        ell(x, y, 2.4, fill=IRON)

def jit_arc(r):
    return 40 + (r * 7) % 53

def hexc(c):
    return (int(c[1:3], 16), int(c[3:5], 16), int(c[5:7], 16))

def mix(c1, c2, t):
    a, b = hexc(c1), hexc(c2)
    return "#%02x%02x%02x" % tuple(int(round(a[k] + (b[k] - a[k]) * t)) for k in range(3))

def moon():
    """A lit sphere with a pitted face - limb light on one side, no rings."""
    RM = random.Random(90210)

    # fake sphere: stacked discs, each smaller and darker, nudged toward the shadow
    N_LAY = 18
    for i in range(N_LAY):
        f = i / (N_LAY - 1)
        r = MOON_R * (1.0 - 0.985 * f)
        dx, dy = -7.0 * f, -5.0 * f
        col = mix("#4a0f0c", "#160304", f ** 0.85)
        ell(CX + dx, CY + dy, r, fill=col)
    draw(circle_d(CX, CY, MOON_R - 1), stroke="#6a1712", w=2.0)

    def clamp_in(pt, rmax):
        dx, dy = pt[0] - CX, pt[1] - CY
        d = math.hypot(dx, dy)
        if d > rmax:
            k = rmax / d
            return (CX + dx * k, CY + dy * k)
        return pt

    # maria: broad, barely darker, soft-edged
    for _ in range(6):
        a = RM.uniform(0, 2 * math.pi)
        d = MOON_R * RM.uniform(0.12, 0.70)
        mx, my = CX + d * math.cos(a), CY + d * math.sin(a)
        rad = MOON_R * RM.uniform(0.16, 0.34)
        pts = []
        n = 20
        for k in range(n):
            th = 2 * math.pi * k / n
            rr = rad * (1 + 0.22 * math.sin(3 * th + RM.uniform(0, 6)) +
                        0.10 * math.sin(5 * th + RM.uniform(0, 6)))
            pts.append(clamp_in((mx + rr * math.cos(th), my + rr * math.sin(th) * 0.9), MOON_R - 4))
        shape(pts, fill="#210506")

    # craters: flat, low contrast, a lit crescent on one side only
    for _ in range(16):
        a = RM.uniform(0, 2 * math.pi)
        d = MOON_R * RM.uniform(0.08, 0.82)
        x, y = CX + d * math.cos(a), CY + d * math.sin(a)
        r = RM.uniform(4.0, 15.0) * (1.0 - 0.4 * d / MOON_R)
        ell(x, y, r, ry=r * 0.95, fill="#1b0405")
        draw(arc_d(x, y, r * 0.86, 195, 340), stroke="#571710", w=max(0.8, r * 0.10))
        draw(arc_d(x, y, r * 0.86, 15, 160), stroke="#140203", w=max(0.7, r * 0.08))

    # a couple of long rilles
    for _ in range(3):
        a = RM.uniform(0, 2 * math.pi)
        r = MOON_R * RM.uniform(0.2, 0.45)
        pts = []
        while r < MOON_R * 0.93:
            pts.append((CX + r * math.cos(a), CY + r * math.sin(a)))
            a += RM.uniform(-0.2, 0.2)
            r += RM.uniform(10, 18)
        stroke_path(pts, "#150304", RM.uniform(0.9, 1.5), cap="round")

    # dust
    parts = []
    for _ in range(200):
        a = RM.uniform(0, 2 * math.pi)
        d = (MOON_R - 4) * math.sqrt(RM.uniform(0, 1))
        x, y = CX + d * math.cos(a), CY + d * math.sin(a)
        r = RM.uniform(0.5, 1.6)
        parts.append("M %.2f %.2f L %.2f %.2f L %.2f %.2f L %.2f %.2f Z" % (
            x - r, y, x, y - r, x + r, y, x, y + r))
    i = oid(cmd("path.create", d=" ".join(parts)))
    ok(cmd("paint.setFill", ids=[i], color="#3a0d09"))
    ok(cmd("paint.setStroke", ids=[i], none=True))

def wound():
    """A sword-cut across the moon: a long parted gash, not a hole."""
    RW = random.Random(4711)
    ax, ay = 0.30, 0.954          # direction of the cut (down and slightly right)
    nx, ny = -ay, ax              # normal across it
    L = 92.0
    N = 34

    def edge(t, side, widen=1.0):
        w = 23.0 * (1.0 - abs(t) ** 1.7) ** 0.62 * (0.78 + 0.42 * math.sin(3.1 * t + 0.6)) * widen
        w += RW.uniform(-2.2, 2.2) if abs(t) < 0.94 else 0.0
        w = max(0.6, w)
        cx_ = CX + ax * L * t
        cy_ = CY + ay * L * t
        return (cx_ + nx * w * side, cy_ + ny * w * side)

    ts = [-1 + 2 * i / N for i in range(N + 1)]
    upper = [edge(t, 1) for t in ts]
    lower = [edge(t, -1) for t in ts]

    # the skin splits: a thin torn halo just outside the cut
    halo_u = [edge(t, 1, 1.30) for t in ts]
    halo_l = [edge(t, -1, 1.30) for t in ts]
    shape(halo_u + halo_l[::-1], fill="#3a0c08")

    # the parted gash itself
    shape(upper + lower[::-1], fill="#1a0303")
    # deeper still in the middle
    mid_u = [edge(t * 0.82, 1, 0.55) for t in ts]
    mid_l = [edge(t * 0.82, -1, 0.55) for t in ts]
    shape(mid_u + mid_l[::-1], fill="#0c0202")

    # a few torn strands, unevenly placed - not a ladder
    for t, off in ((-0.34, 3.0), (0.08, -4.0)):
        a_ = edge(t, 1, 0.9)
        b_ = edge(t + RW.uniform(-0.10, 0.10), -1, 0.9)
        m_ = ((a_[0] + b_[0]) / 2 + RW.uniform(-6, 6), (a_[1] + b_[1]) / 2 + RW.uniform(-6, 6))
        stroke_path([a_, m_, b_], "#5e241a", RW.uniform(0.9, 1.4), cap="round")
    for k in range(5):   # short nicks of tissue instead of full bridges
        t = RW.uniform(-0.7, 0.7)
        side = RW.choice((1, -1))
        p0 = edge(t, side, 0.55)
        p1 = edge(t + RW.uniform(0.03, 0.10), side, 0.95)
        stroke_path([p0, p1], "#4a1a13", RW.uniform(0.8, 1.3), cap="round")

    # torn flaps of skin along the edges
    for k in range(8):
        t = RW.uniform(-0.85, 0.85)
        side = RW.choice((1, -1))
        p0 = edge(t, side, 0.95)
        p1 = edge(t, side, 1.35)
        p2 = edge(t + RW.uniform(0.04, 0.12), side, 1.05)
        shape([p0, p1, p2], fill="#4c110c")

    # a wet lip: bright on the light side, dark on the other
    stroke_path(upper, "#8a2f20", 1.3, cap="round")
    stroke_path(lower, "#2a0706", 2.0, cap="round")

    # inner wall catching a little light, so the gash has depth
    stroke_path([edge(t, 1, 0.62) for t in ts[6:-6]], "#3a0d09", 1.2, cap="round")
    stroke_path([edge(t, -1, 0.58) for t in ts[8:-8]], "#2a0706", 1.0, cap="round")

    # blood standing in the lower part of the cut: a simple body with a wet surface
    k0, k1 = 12, N - 2
    surf, low = [], []
    for i in range(k0, k1 + 1):
        f = 0.26 + 0.10 * math.sin(i * 0.7 + 1.0)
        u, l = upper[i], lower[i]
        surf.append((l[0] + (u[0] - l[0]) * f, l[1] + (u[1] - l[1]) * f))
        low.append(l)
    # a liquid surface is level: interpolate across the cut instead of following the lips
    y0, y1 = surf[0][1], surf[-1][1]
    surf_s = [(x, y0 + (y1 - y0) * (i / (len(surf) - 1)))
              for i, (x, y) in enumerate(surf)]
    shape(smooth(low, 6) + surf_s[::-1], fill="#260404")
    # the moon's rim caught in the standing blood: a muted band under the surface
    refl = [(x, y + 4.0) for x, y in surf_s]
    shape(surf_s + refl[::-1], fill="#4a1411")
    # sharp specular along the liquid surface
    stroke_path(surf_s[8:-8], "#a2443a", 0.9, cap="butt")
    # it runs out of the bottom of the cut
    bx, by = low[-1]
    shape([(bx - 3.0, by - 6), (bx + 3.0, by - 6), (bx + 2.2, by + 7), (bx - 2.2, by + 7)],
          fill="#260404")

def frame(a_deg):
    """Local -> world for one sword: guard sits at R_GUARD, blade runs inward."""
    a = math.radians(a_deg)
    ux, uy = math.cos(a), math.sin(a)
    rx, ry = -uy, ux
    def T(x, y):
        return (CX + ux * (R_GUARD + y) + rx * x, CY + uy * (R_GUARD + y) + ry * x)
    return T

def sword(a_deg, k):
    a = math.radians(a_deg)
    ux, uy = math.cos(a), math.sin(a)
    rx, ry = -uy, ux
    T = lambda x, y: (CX + ux * (R_GUARD + y) + rx * x, CY + uy * (R_GUARD + y) + ry * x)
    w, L = BLADE_W, BLADE_L
    gx = 33.0

    # blade
    P = Path(T)
    P.M(-w, 0).L(-w * 0.97, -L * 0.55)
    P.C(-w * 0.93, -L * 0.73, -w * 0.48, -L * 0.90, 0, -L)
    P.C(w * 0.48, -L * 0.90, w * 0.93, -L * 0.73, w * 0.97, -L * 0.55)
    P.L(w, 0).Z()
    draw(P.d(), fill=STEEL, stroke=IRON, w=1.4, join="round")

    # forged shading down the right half
    P = Path(T)
    P.M(w * 0.60, 0).L(w * 0.62, -L * 0.52)
    P.C(w * 0.60, -L * 0.72, w * 0.44, -L * 0.86, 0, -L)
    P.C(w * 0.48, -L * 0.90, w * 0.93, -L * 0.73, w * 0.97, -L * 0.55)
    P.L(w, 0).Z()
    draw(P.d(), fill=STEEL_D)

    # fuller
    P = Path(T)
    P.M(-w * 0.30, -L * 0.06).L(-w * 0.24, -L * 0.54)
    P.C(-w * 0.13, -L * 0.61, w * 0.13, -L * 0.61, w * 0.24, -L * 0.54)
    P.L(w * 0.30, -L * 0.06)
    P.C(w * 0.12, -L * 0.02, -w * 0.12, -L * 0.02, -w * 0.30, -L * 0.06)
    P.Z()
    draw(P.d(), fill=IRON_HI, stroke=ENGRAVE, w=0.9)

    # crossguard: quillons swept toward the point, ball terminals
    P = Path(T)
    P.M(0, -7)
    P.C(gx * 0.35, -11, gx * 0.70, -15, gx, -24)
    P.C(gx + 6, -26, gx + 7, -13, gx, -10)
    P.C(gx * 0.66, -7, gx * 0.36, 3, 0, 5)
    P.C(-gx * 0.36, 3, -gx * 0.66, -7, -gx, -10)
    P.C(-(gx + 7), -13, -(gx + 6), -26, -gx, -24)
    P.C(-gx * 0.70, -15, -gx * 0.35, -11, 0, -7)
    P.Z()
    draw(P.d(), fill=STEEL, stroke=IRON, w=1.4, join="round")

    # grip
    P = Path(T)
    P.M(-7, 6).L(-6, 42).C(-3.5, 47, 3.5, 47, 6, 42).L(7, 6).Z()
    draw(P.d(), fill=IRON_HI, stroke=ENGRAVE, w=1.1, join="round")
    for j in range(4):
        y = 12 + 7.5 * j
        P = Path(T)
        P.M(-6.4, y).L(6.4, y - 3.4)
        draw(P.d(), stroke=ENGRAVE, w=1.0)

    # pommel: a wheel with a dark eye
    px, py = T(0, 54)
    ell(px, py, 9.5, fill=STEEL, stroke=IRON, w=1.3)
    ell(px, py, 3.6, fill=IRON_HI)
    P = Path(T)
    P.M(-9.5, 54).L(-13, 54).M(9.5, 54).L(13, 54)
    draw(P.d(), stroke=STEEL, w=1.6)

    # lit edge down the left of the blade
    P = Path(T)
    P.M(-w * 0.86, -6).L(-w * 0.84, -L * 0.55)
    P.C(-w * 0.80, -L * 0.70, -w * 0.42, -L * 0.86, 0, -L * 0.97)
    draw(P.d(), stroke=STEEL_HI, w=1.3)

def resample(pts, n):
    seg = [0.0]
    for k in range(1, len(pts)):
        seg.append(seg[-1] + math.hypot(pts[k][0] - pts[k - 1][0], pts[k][1] - pts[k - 1][1]))
    total = seg[-1] or 1.0
    out, j = [], 0
    for i in range(n):
        target = total * i / (n - 1)
        while j < len(seg) - 2 and seg[j + 1] < target:
            j += 1
        span = (seg[j + 1] - seg[j]) or 1.0
        f = (target - seg[j]) / span
        out.append((pts[j][0] + (pts[j + 1][0] - pts[j][0]) * f,
                    pts[j][1] + (pts[j + 1][1] - pts[j][1]) * f))
    return out

def blobpath(items, colour):
    """Many discs in a single path object - keeps the document light."""
    parts = []
    for x, y, rx, ry in items:
        pts = []
        for k in range(10):
            th = 2 * math.pi * k / 10
            pts.append((x + rx * math.cos(th), y + ry * math.sin(th)))
        parts.append("M " + " L ".join("%.2f %.2f" % q for q in pts) + " Z")
    i = oid(cmd("path.create", d=" ".join(parts)))
    ok(cmd("paint.setFill", ids=[i], color=colour))
    ok(cmd("paint.setStroke", ids=[i], none=True))
    return i

def liquid(pts, r0, r1, n=40, amp=0.22, phase=0.0, tone=None, hi_col=None, hi_w=0.9):
    """Blood as a smooth painted stroke: dark skin, body, a wet sliver."""
    line = resample(smooth(pts, 12), n)
    widths = []
    for i in range(n):
        t = i / (n - 1)
        r = (r0 + (r1 - r0) * t) * (1.0 + amp * math.sin(t * 6.3 + phase))
        widths.append(max(1.0, r))

    def band(scale, colour, grow=0.0):
        left, right = [], []
        for i, (x, y) in enumerate(line):
            x0, y0 = line[max(0, i - 1)]
            x1, y1 = line[min(n - 1, i + 1)]
            dx, dy = x1 - x0, y1 - y0
            L = math.hypot(dx, dy) or 1.0
            ux, uy = -dy / L, dx / L
            w = widths[i] * scale + grow
            left.append((x + ux * w, y + uy * w))
            right.append((x - ux * w, y - uy * w))
        return shape(left + right[::-1], fill=colour)

    band(1.22, BLOOD_D)
    band(0.94, tone or BLOOD)
    # wet sliver: a thin light line riding one side of the run
    hi = []
    for i in range(1, n - 1):
        x0, y0 = line[i - 1]
        x1, y1 = line[i + 1]
        dx, dy = x1 - x0, y1 - y0
        L = math.hypot(dx, dy) or 1.0
        off = widths[i] * 0.42
        hi.append((line[i][0] - dy / L * off, line[i][1] + dx / L * off))
    if len(hi) > 6:
        stroke_path(hi[2:-3], hi_col or BLOOD_HI, hi_w, cap="round")
    return line, widths

def bead(x, y, r, tone=BLOOD):
    ell(x, y, r * 1.18, ry=r * 1.24, fill=BLOOD_D)
    ell(x, y, r, ry=r * 1.1, fill=tone)
    ell(x - r * 0.34, y - r * 0.24, r * 0.26, ry=r * 0.36, fill=BLOOD_HI)

def drop_from(x, y, ln, w, r_bead, phase=0.0):
    """One continuous shape: the thread leaves the surface and swells into a drop."""
    sway = math.sin(phase) * 2.2
    yb = y + ln
    P = Path(lambda a, b: (a, b))
    P.M(x - w, y)
    P.C(x - w * 0.7, y + ln * 0.45, x - r_bead * 0.85 + sway, yb - r_bead * 0.75,
        x - r_bead, yb)
    P.C(x - r_bead * 1.02, yb + r_bead * 1.15, x + r_bead * 1.02, yb + r_bead * 1.15,
        x + r_bead, yb)
    P.C(x + r_bead * 0.85 + sway, yb - r_bead * 0.75, x + w * 0.7, y + ln * 0.45, x + w, y)
    P.Z()
    draw(P.d(), fill=BLOOD)
    P = Path(lambda a, b: (a, b))
    P.M(x - r_bead * 0.92, yb + r_bead * 0.15)
    P.C(x - r_bead * 0.9, yb + r_bead * 1.0, x + r_bead * 0.9, yb + r_bead * 1.0,
        x + r_bead * 0.92, yb + r_bead * 0.15)
    draw(P.d(), stroke=BLOOD_D, w=1.4, cap="round")
    ell(x - r_bead * 0.34 + sway * 0.4, yb + r_bead * 0.05, r_bead * 0.26,
        ry=r_bead * 0.34, fill=BLOOD_HI)

def wound_blood():
    """Blood seeping out of the star and running down the face of the moon."""
    def rim(a_deg):
        a = math.radians(a_deg)
        return (CX + (MOON_R - 1) * math.cos(a), CY + (MOON_R - 1) * math.sin(a))

    # the seep: a dark crescent hugging the star, one wet point of light
    P = Path(lambda p, q: (p, q))
    P.M(CX - 13, CY + 44)
    P.C(CX - 15, CY + 54, CX + 13, CY + 56, CX + 12, CY + 44)
    P.C(CX + 8, CY + 50, CX - 8, CY + 49, CX - 13, CY + 44)
    P.Z()
    draw(P.d(), fill=BLOOD_D)
    ell(CX - 1, CY + 48, 6.0, ry=3.4, fill=BLOOD)
    ell(CX - 3, CY + 47, 1.8, ry=1.1, fill=BLOOD_HI)

    liquid([(CX + 14, 586), (CX - 14, 604), (CX - 40, 620), (CX - 56, 640), rim(117)],
           1.8, 2.5, n=30, amp=0.36, phase=0.7)
    liquid([(CX + 26, 584), (CX + 42, 600), (CX + 56, 618), (CX + 64, 634), rim(59)],
           1.4, 2.0, n=26, amp=0.36, phase=2.1)
    liquid([(CX + 20, 590), (CX + 18, 606), (CX + 10, 620), (CX + 2, 630)],
           1.0, 1.4, n=16, amp=0.4, phase=4.0)

def pool():
    """Blood standing in the channel of the ring: skin, body, a broken top sheen."""
    def P(a_deg, r):
        a = math.radians(a_deg)
        return (CX + r * math.cos(a), CY + r * math.sin(a))

    a0, a1 = 46.0, 134.0

    def lens(d_out, d_in, colour, steps=44):
        outer, inner = [], []
        for i in range(steps + 1):
            f = i / steps
            a = a0 + (a1 - a0) * f
            th = (math.sin(math.pi * f) ** 0.62) * 18.0 + 1.0
            outer.append(P(a, 374.0 + th - d_out))
            inner.append(P(a, 374.0 - th * 0.80 + 1.4 * math.sin(a * 2.1) + d_in))
        shape(smooth(outer, 4) + smooth(inner, 4)[::-1], fill=colour)

    lens(0.0, 0.0, BLOOD_D)          # contact shadow
    lens(2.6, 2.4, BLOOD)            # body
    lens(4.4, 7.0, "#4a0807")        # the deep middle of the puddle

    # top sheen, broken into short arcs so it never reads as a drawn line
    top = []
    for i in range(49):
        f = i / 48
        a = a0 + (a1 - a0) * f
        th = (math.sin(math.pi * f) ** 0.62) * 18.0 + 1.0
        top.append(P(a, 374.0 + th - 5.0))
    for a, b in ((6, 16), (20, 29), (33, 41)):
        stroke_path(top[a:b], BLOOD_HI, 1.1, cap="round")

    # where the pool laps at the ends
    for a, sgn in ((a0 + 1.0, -1), (a1 - 1.0, 1)):
        pts = [P(a + sgn * k * 1.4, 374 + 4 * math.sin(k * 0.8)) for k in range(6)]
        liquid(pts, 2.4, 1.0, n=14, amp=0.2, phase=a)

def blade_blood():
    """Swords dipped in blood: a ragged red film over the point end of some blades."""
    R = random.Random(1977)
    for k in range(N):
        a_deg = -90 + 45 * k
        T = frame(a_deg)
        w, L = BLADE_W, BLADE_L
        t_dip = R.uniform(0.14, 0.58)
        M = 14
        ts = [t_dip + (1.0 - t_dip) * i / (M - 1) for i in range(M)]

        def edge(t, s):
            we = w * (1 - t * 0.95)
            return (s * we * 0.88, -L * t)

        left = [edge(t, -1) for t in ts]
        right = [edge(t, 1) for t in reversed(ts)]

        # ragged line where the film stops, with a couple of tongues running further up
        lip = []
        n_lip = 9
        for i in range(n_lip):
            f = i / (n_lip - 1)
            x = (1 - 2 * f) * w * (1 - t_dip * 0.95) * 0.9
            dy = L * R.uniform(-0.035, 0.075)
            lip.append((x, -L * t_dip + dy))
        P = Path(T)
        P.M(*left[0])
        for p in left[1:]:
            P.L(*p)
        for p in right:
            P.L(*p)
        for p in lip:
            P.L(*p)
        P.Z()
        draw(P.d(), fill=BLOOD)
        # a dark seam at the lip so the film sits on the steel instead of on top of it
        P = Path(T)
        P.M(*lip[0])
        for p in lip[1:]:
            P.L(*p)
        draw(P.d(), stroke=BLOOD_D, w=2.0, join="round")
        # two darker threads inside the film, like runs that started separately
        for _ in range(R.randint(1, 2)):
            tt = [R.uniform(t_dip, 0.97) for _ in range(3)]
            tt.sort()
            tr = [(R.uniform(-0.55, 0.55) * w * (1 - t * 0.95), -L * t) for t in tt]
            P = Path(T)
            P.M(*tr[0])
            for p in tr[1:]:
                P.L(*p)
            draw(P.d(), stroke=BLOOD_D, w=1.4, cap="round")

        # droplets thrown up the clean part of the blade
        for _ in range(R.randint(1, 3)):
            t = R.uniform(max(0.05, t_dip - 0.32), t_dip - 0.02)
            we = w * (1 - t * 0.95)
            x, y = T(R.uniform(-0.7, 0.7) * we, -L * t)
            rr = R.uniform(1.4, 2.8)
            ell(x, y, rr, ry=R.uniform(1.1, 2.0), fill=BLOOD)
            ell(x, y, rr * 0.5, ry=rr * 0.5, fill=BLOOD_D)


def rim_blood():
    """Where the runs go over the moon's edge: a thread and a drop, nothing in flight."""
    def rim(a_deg):
        a = math.radians(a_deg)
        return (CX + (MOON_R - 1) * math.cos(a), CY + (MOON_R - 1) * math.sin(a))

    for a_deg, ln, w, rb, ph in ((119, 74, 2.6, 6.6, 0.4), (61, 58, 2.2, 5.6, 1.9),
                                 (134, 26, 1.6, 3.4, 3.0)):
        x, y = rim(a_deg)
        # the lip it clings to
        blobpath([(x + RS.uniform(-2, 2), y + RS.uniform(-1.5, 1),
                   RS.uniform(1.8, 3.0), RS.uniform(1.2, 2.2)) for _ in range(4)], BLOOD)
        drop_from(x, y + 2, ln, w, rb, phase=ph)

def overflow():
    """The pool has filled the channel: blood creeps over the outer rim and drips off."""
    def P(a_deg, r):
        a = math.radians(a_deg)
        return (CX + r * math.cos(a), CY + r * math.sin(a))

    for a_deg, ln, w, rb, ph in ((76, 88, 2.2, 6.6, 0.8), (97, 58, 2.0, 5.4, 2.7),
                                 (117, 32, 1.6, 3.8, 4.4)):
        # the lip of blood crawling over the band's outside face
        liquid([P(a_deg - 0.6, 388), P(a_deg, 396), P(a_deg, 404)], 4.8, 3.2, n=16, amp=0.16,
               phase=ph)
        x, y = P(a_deg, 404)
        drop_from(x, y + 2, ln, w, rb, phase=ph)
        if ln > 70:   # one that already let go
            ell(x, y + ln + rb + 46, 2.6, ry=3.8, fill=BLOOD)
            ell(x - 0.8, y + ln + rb + 44, 0.9, ry=1.4, fill=BLOOD_HI)

def build():
    ok(cmd("file.new"))
    ok(cmd("artboard.setProps", index=0, width=1024, height=1024, name="Sigil"))
    field()
    band()
    moon()
    wound()
    wound_blood()
    pool()
    for k in range(N):
        sword(-90 + 45 * k, k)
    blade_blood()
    rim_blood()
    overflow()

def main():
    build()
    name = "afterglow-sigil.svg"
    info = ok(cmd("document.export", path=os.path.join(OUT, name), format="svg", artboard=0, scale=1.0))
    print("%s %d bytes" % (name, info["bytes"]))

if __name__ == "__main__":
    main()
