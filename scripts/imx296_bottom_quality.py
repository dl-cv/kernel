#!/usr/bin/env python3
"""IMX296 bottom-band quality check (classic green / chroma seam / tear).

Designed to catch:
  - classic G-dominant green band (incomplete UV green cast)
  - #1013-style flat neutral-grey seam (UV forced 0x80)
  - bottom tear / row discontinuity (abrupt band vs scene)

Pure stdlib + Pillow (no numpy required) so it runs on board CamOS images.
"""
from __future__ import annotations

from io import BytesIO
from typing import Any

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None  # type: ignore

BOT_ROWS = 8
# Skip transition / UV-footprint zone above bot when sampling reference.
REF_SKIP = 8
REF_ROWS = 8

# classic green
G_DELTA = 20.0
G_ABS = 80.0
LAST_G_DELTA = 25.0

# seam: flat achromatic bot vs colored scene
GRAY_CH_MAX = 7.0
FLAT_STD_MAX = 5.5
SEAM_L1_MIN = 3.0
SEAM_SAT_MIN = 5.5
SEAM_LUMA_MAX = 90.0

# tear: abrupt discontinuity even if not grey
TEAR_L1_MIN = 10.0
TEAR_ROW_JUMP = 12.0  # mean |row_i - row_{i-1}| across RGB
TEAR_BOT_STD_MAX = 18.0  # tear band itself not wildly textured noise


def _mean_std(vals: list[float]) -> tuple[float, float]:
    n = len(vals) or 1
    m = sum(vals) / n
    if n < 2:
        return m, 0.0
    var = sum((v - m) ** 2 for v in vals) / n
    return m, var ** 0.5


def _band_stats(pix, w: int, y0: int, y1: int, x_step: int = 2) -> dict[str, float]:
    rs: list[float] = []
    gs: list[float] = []
    bs: list[float] = []
    y0 = max(0, y0)
    y1 = max(y0, y1)
    for y in range(y0, y1):
        for x in range(0, w, x_step):
            r, g, b = pix[x, y][:3]
            rs.append(float(r))
            gs.append(float(g))
            bs.append(float(b))
    if not rs:
        return {"r": 0.0, "g": 0.0, "b": 0.0, "std": 0.0, "sat": 0.0, "luma": 0.0}
    mr, sr = _mean_std(rs)
    mg, sg = _mean_std(gs)
    mb, sb = _mean_std(bs)
    sat = max(abs(mr - mg), abs(mg - mb), abs(mr - mb))
    luma = 0.299 * mr + 0.587 * mg + 0.114 * mb
    return {
        "r": mr,
        "g": mg,
        "b": mb,
        "std": max(sr, sg, sb),
        "sat": sat,
        "luma": luma,
    }


def _row_mean(pix, w: int, y: int, x_step: int = 4) -> tuple[float, float, float]:
    rs, gs, bs = [], [], []
    for x in range(0, w, x_step):
        r, g, b = pix[x, y][:3]
        rs.append(float(r))
        gs.append(float(g))
        bs.append(float(b))
    n = len(rs) or 1
    return sum(rs) / n, sum(gs) / n, sum(bs) / n


def analyze_image(im: "Image.Image") -> dict[str, Any]:
    """Analyze bottom quality of an RGB PIL image."""
    im = im.convert("RGB")
    w, h = im.size
    pix = im.load()
    n = min(BOT_ROWS, h)

    bot = _band_stats(pix, w, h - n, h)
    skip = min(REF_SKIP, max(0, h - 2 * n))
    a1 = h - n - skip
    a0 = max(0, a1 - REF_ROWS)
    above = _band_stats(pix, w, a0, a1) if a1 > a0 else bot
    mid = _band_stats(pix, w, h // 2, h // 2 + 1)

    br, bg, bb = bot["r"], bot["g"], bot["b"]
    ar, ag, ab = above["r"], above["g"], above["b"]
    mr, mg, mb = mid["r"], mid["g"], mid["b"]

    # --- classic green (bottom band vs scene; ignore global green cast) ---
    classic = False
    reasons: list[str] = []
    mean_l1 = (abs(br - ar) + abs(bg - ag) + abs(bb - ab)) / 3.0
    mid_l1 = (abs(br - mr) + abs(bg - mg) + abs(bb - mb)) / 3.0
    bot_g_ex = bg - max(br, bb)
    above_g_ex = ag - max(ar, ab)
    mid_g_ex = mg - max(mr, mb)
    g_dom = bg > br + G_DELTA and bg > bb + G_DELTA
    # True incomplete-UV stripe: bot more G-dominant than above/mid.
    if g_dom and bot_g_ex >= above_g_ex + 12 and bot_g_ex >= mid_g_ex + 8:
        classic = True
        reasons.append("g_delta_band")
    if (
        bg > G_ABS
        and br < 55
        and bb < 55
        and (mean_l1 >= 3.0 or bot_g_ex >= above_g_ex + 15)
    ):
        classic = True
        reasons.append("pure_g")
    lr, lg, lb = _row_mean(pix, w, h - 1)
    if (
        lg > lr + LAST_G_DELTA
        and lg > lb + LAST_G_DELTA
        and (lg - max(lr, lb)) >= (ag - max(ar, ab)) + 10
    ):
        classic = True
        reasons.append("last_row_g")

    # --- seam: flat grey bot vs colored scene ---
    bot_gray = (
        abs(br - bg) <= GRAY_CH_MAX
        and abs(bg - bb) <= GRAY_CH_MAX
        and abs(br - bb) <= GRAY_CH_MAX
    )
    bot_flat = bot["std"] <= FLAT_STD_MAX
    scene_sat = max(above["sat"], mid["sat"])
    seam = False
    if (
        bot_flat
        and bot_gray
        and scene_sat >= SEAM_SAT_MIN
        and mean_l1 >= SEAM_L1_MIN
        and bot["luma"] <= SEAM_LUMA_MAX
    ):
        seam = True
        reasons.append("seam_above")
    if (
        not seam
        and bot_flat
        and bot_gray
        and mid["sat"] >= SEAM_SAT_MIN
        and mid_l1 >= SEAM_L1_MIN
        and bot["luma"] <= SEAM_LUMA_MAX
    ):
        seam = True
        reasons.append("seam_mid")

    # --- tear: abrupt discontinuity (not necessarily grey) ---
    # 1) band L1 vs above
    # 2) max adjacent-row jump inside last 16 rows
    max_jump = 0.0
    y_start = max(0, h - max(16, 2 * n))
    prev = _row_mean(pix, w, y_start)
    for y in range(y_start + 1, h):
        cur = _row_mean(pix, w, y)
        jump = (abs(cur[0] - prev[0]) + abs(cur[1] - prev[1]) + abs(cur[2] - prev[2])) / 3.0
        if jump > max_jump:
            max_jump = jump
        prev = cur

    tear = False
    if (
        mean_l1 >= TEAR_L1_MIN
        and bot["std"] <= TEAR_BOT_STD_MAX
        and scene_sat >= 4.0
        and max_jump >= TEAR_ROW_JUMP
    ):
        # avoid flagging classic green twice; still record tear if not classic-only
        tear = True
        reasons.append("tear_jump")
    # strong L1 discontinuity without needing jump (sanitize hard edge)
    if (
        not tear
        and mean_l1 >= TEAR_L1_MIN + 4
        and bot_flat
        and scene_sat >= SEAM_SAT_MIN
        and not classic
    ):
        tear = True
        reasons.append("tear_l1")

    bad = bool(classic or seam or tear)
    # severity score 0..100 for ranking dumps
    score = 0.0
    if classic:
        score += 40 + min(40.0, max(0.0, bg - max(br, bb)))
    if seam:
        score += 35 + min(25.0, mean_l1 * 3)
    if tear:
        score += 25 + min(20.0, max_jump)
    score = min(100.0, score)

    # center luma for phase (endurance compatibility)
    ch = max(8, int(h * 0.25))
    cw = max(8, int(w * 0.25))
    y0 = (h - ch) // 2
    x0 = (w - cw) // 2
    center = _band_stats(pix, w, y0, y0 + ch, x_step=4)
    # approximate center luma already in center['luma'] but band_stats uses full width step;
    # recompute tighter:
    cs = []
    for y in range(y0, y0 + ch):
        for x in range(x0, x0 + cw, 4):
            r, g, b = pix[x, y][:3]
            cs.append(0.299 * r + 0.587 * g + 0.114 * b)
    mean = sum(cs) / len(cs) if cs else center["luma"]

    return {
        "dim": f"{w}x{h}",
        "mean": float(mean),
        "br": round(br, 1),
        "bg": round(bg, 1),
        "bb": round(bb, 1),
        "mr": round(mr, 1),
        "mg": round(mg, 1),
        "mb": round(mb, 1),
        "ar": round(ar, 1),
        "ag": round(ag, 1),
        "ab": round(ab, 1),
        "bot_std": round(bot["std"], 2),
        "above_sat": round(above["sat"], 2),
        "mid_sat": round(mid["sat"], 2),
        "mean_l1": round(mean_l1, 2),
        "mid_l1": round(mid_l1, 2),
        "max_row_jump": round(max_jump, 2),
        "classic_green": bool(classic),
        "seam": bool(seam),
        "tear": bool(tear),
        "flat_gray": bool(bot_flat and bot_gray),
        "reasons": reasons,
        "score": round(score, 1),
        # endurance / legacy
        "green": bad,
        "bad": bad,
    }


def analyze_jpeg(data: bytes) -> dict[str, Any]:
    if Image is None:
        raise RuntimeError("Pillow required")
    return analyze_image(Image.open(BytesIO(data)))


def analyze_path(path: str) -> dict[str, Any]:
    if Image is None:
        raise RuntimeError("Pillow required")
    return analyze_image(Image.open(path))
