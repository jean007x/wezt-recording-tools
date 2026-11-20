#!/usr/bin/env python3
"""Re-traces assets/logo_original.jpg as clean vector geometry and writes src/ui/logo_mask.inc.

The scan has wobble (dents in straight edges, kinks where thin curves meet blocks). Smoothing the picture cannot remove that, so:
  1. threshold at 3x resolution, find every outline,
  2. split each outline at its real corners (sharp turns),
  3. between corners: an edge that is nearly straight becomes an exactly straight line; any other edge is smoothed along its length
     (ends pinned) so curves flow evenly,
  4. where two straight edges meet, the corner is moved to the exact intersection of the two lines (crisp, square corners),
  5. fill the outlines (and thicken them a little with an outline stroke), downsample with anti-aliasing.
Run:  python3 tools/make_logo_mask.py        (needs numpy, opencv-python, pillow)
"""
import sys
import cv2
import numpy as np
from PIL import Image

SRC, OUT = 'assets/logo_original.jpg', 'src/ui/logo_mask.inc'
UP = 3                # working resolution factor
W_OUT = 1024          # width of the stored mask
GROW = 12             # extra stroke thickness at working resolution (about 2 px each side in the mask)
STEP = 1.0            # resampling step along an outline, working pixels
CORNER_K = 14         # window (samples) used to measure how sharply an outline turns
CORNER_DEG = 46.0     # turning angle that counts as a corner
SMOOTH_SIGMA = 34.0   # smoothing along curved edges, samples
STRAIGHT_TOL = 10.0    # an edge whose farthest point is this close to the chord is a straight line

def gauss_kernel(sigma):
    r = int(3 * sigma)
    x = np.arange(-r, r + 1)
    k = np.exp(-0.5 * (x / sigma) ** 2)
    return k / k.sum(), r

def resample(P, step):
    d = np.r_[0, np.cumsum(np.hypot(*np.diff(np.vstack([P, P[0]]), axis=0).T))]
    n = max(8, int(d[-1] / step))
    t = np.linspace(0, d[-1], n, endpoint=False)
    Q = np.vstack([P, P[0]])
    return np.c_[np.interp(t, d, Q[:, 0]), np.interp(t, d, Q[:, 1])]

def circ_smooth(P, sigma):
    k, r = gauss_kernel(sigma)
    n = len(P)
    idx = (np.arange(-r, n + r)) % n
    out = np.empty_like(P)
    for c in range(2):
        out[:, c] = np.convolve(P[idx, c], k, mode='valid')
    return out

def find_corners(P):
    n = len(P)
    Ps = circ_smooth(P, 3.0)
    a = Ps - np.roll(Ps, CORNER_K, axis=0)
    b = np.roll(Ps, -CORNER_K, axis=0) - Ps
    cross = a[:, 0] * b[:, 1] - a[:, 1] * b[:, 0]
    dot = (a * b).sum(axis=1)
    ang = np.degrees(np.abs(np.arctan2(cross, dot)))
    cand = [i for i in range(n) if ang[i] > CORNER_DEG and ang[i] >= ang[(np.arange(i - CORNER_K, i + CORNER_K + 1)) % n].max() - 1e-9]
    out = []
    for i in cand:                                   # keep one corner per neighbourhood
        if not out or min(abs(i - j) % n if abs(i - j) % n < n / 2 else n - abs(i - j) % n for j in out) > CORNER_K:
            out.append(i)
    return sorted(out)

def smooth_open(S, sigma):
    """Smooth an open run with both ends pinned (the ends are reflected through themselves so the tangent is kept)."""
    n = len(S)
    if n < 6: return S
    k, r = gauss_kernel(min(sigma, n / 3.0))
    r = min(r, n - 1)
    k = k[len(k) // 2 - r: len(k) // 2 + r + 1]; k = k / k.sum()
    head = 2 * S[0] - S[1:r + 1][::-1]
    tail = 2 * S[-1] - S[-r - 1:-1][::-1]
    E = np.vstack([head, S, tail])
    out = np.stack([np.convolve(E[:, c], k, mode='valid') for c in range(2)], axis=1)
    out[0], out[-1] = S[0], S[-1]
    return out

def line_isect(p1, d1, p2, d2):
    A = np.array([d1, -d2]).T
    if abs(np.linalg.det(A)) < 1e-6: return None
    t = np.linalg.solve(A, p2 - p1)
    return p1 + t[0] * d1

def trace(P):
    P = resample(P, STEP)
    n = len(P)
    cs = find_corners(P)
    if len(cs) < 2:
        return circ_smooth(P, SMOOTH_SIGMA), []
    segs, straight = [], []
    for a, b in zip(cs, cs[1:] + [cs[0] + n]):
        idx = np.arange(a, b + 1) % n
        S = P[idx]
        chord = S[-1] - S[0]; L = np.hypot(*chord)
        if L < 1e-6: segs.append(S); straight.append(False); continue
        dev = np.abs((S[:, 0] - S[0, 0]) * chord[1] - (S[:, 1] - S[0, 1]) * chord[0]) / L
        if dev.max() < STRAIGHT_TOL and L > 6:
            segs.append(np.vstack([S[0], S[-1]])); straight.append(True)
        else:
            segs.append(smooth_open(S, SMOOTH_SIGMA)); straight.append(False)
    # crisp corners where two straight edges meet
    m = len(segs)
    for i in range(m):
        j = (i + 1) % m
        if straight[i] and straight[j]:
            p1, p2 = segs[i][0], segs[i][-1]
            q1, q2 = segs[j][0], segs[j][-1]
            X = line_isect(p1, p2 - p1, q1, q2 - q1)
            if X is not None and np.hypot(*(X - p2)) < 14:
                segs[i] = np.vstack([segs[i][0], X]); segs[j] = np.vstack([X, segs[j][-1]])
    return np.vstack([s[:-1] for s in segs]), cs

def main():
    im = Image.open(SRC).convert('L')
    w, h = im.size
    big = np.asarray(im.resize((w * UP, h * UP), Image.BICUBIC))
    binary = (big < 128).astype(np.uint8) * 255
    cnts, hier = cv2.findContours(binary, cv2.RETR_CCOMP, cv2.CHAIN_APPROX_NONE)
    canvas = np.zeros(binary.shape, np.uint8)
    SH = 4
    def poly(img, pts, color):
        q = np.round(pts * (1 << SH)).astype(np.int32).reshape(-1, 1, 2)
        cv2.fillPoly(img, [q], color, lineType=cv2.LINE_AA, shift=SH)
    def stroke(img, pts, color, th):
        q = np.round(pts * (1 << SH)).astype(np.int32).reshape(-1, 1, 2)
        cv2.polylines(img, [q], True, color, th, lineType=cv2.LINE_AA, shift=SH)
    traced = {}
    for i, c in enumerate(cnts):
        if cv2.contourArea(c) < 150: continue
        traced[i] = trace(c[:, 0, :].astype(np.float64))[0]
    for i, pts in traced.items():
        if hier[0][i][3] != -1: continue                  # holes are handled with their parent
        tmp = np.zeros_like(canvas)
        poly(tmp, pts, 255)
        for j, hp in traced.items():
            if hier[0][j][3] == i: poly(tmp, hp, 0)
        canvas = np.maximum(canvas, tmp)
    # thicken: stroke every outline (outer outlines grow the shape, hole outlines shrink the holes)
    th = np.zeros_like(canvas)
    for pts in traced.values(): stroke(th, pts, 255, GROW)
    canvas = np.maximum(canvas, th)
    ys, xs = np.where(canvas > 12)
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    pad = 20
    canvas = canvas[max(0, y0 - pad): y1 + pad, max(0, x0 - pad): x1 + pad]
    img = Image.fromarray(canvas)
    H = round(img.height * W_OUT / img.width)
    img = img.resize((W_OUT, H), Image.LANCZOS)
    d = np.asarray(img, dtype=np.uint8)
    with open(OUT, 'w') as f:
        f.write("// Generated by tools/make_logo_mask.py from assets/logo_original.jpg (the project owner's logo): 8-bit coverage, %dx%d, row-major.\n"
                "// The scan was re-traced as clean geometry: straight edges are exactly straight, corners are exact, curves are smoothed along their length.\n" % (W_OUT, H))
        f.write('constexpr int kLogoMaskW = %d, kLogoMaskH = %d;\nconstexpr unsigned char kLogoMask[] = {\n' % (W_OUT, H))
        flat = d.flatten()
        for i in range(0, len(flat), 40):
            f.write(','.join(str(int(v)) for v in flat[i:i + 40]) + ',\n')
        f.write('};\n')
    Image.fromarray(d).save(sys.argv[1] if len(sys.argv) > 1 else '/dev/null', format='PNG') if len(sys.argv) > 1 else None
    print('wrote', OUT, W_OUT, H, 'contours traced:', len(traced))

main()
