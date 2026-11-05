#!/usr/bin/env python3
"""Extra checks beyond verify_accept_user.py (originally 148).

Covers panorama §4 / defect E (Y tear at gain=240 1500↔15000),
F idle line0 (no spurious +1), J n1trace default, I pulse restore,
and whole-frame olive (WB/dirty) on head frames.

CamOS 控制面默认 /ws/cmd；账号用 CAMOS_USER / CAMOS_PASS。
"""
from __future__ import annotations

import asyncio
import importlib.util
import json
import os
import sys
import time
from pathlib import Path

import requests
import websockets

BASE = os.environ.get("CAMOS_BASE", "http://127.0.0.1:8000")
WS_URL = os.environ.get("CAMOS_WS", "ws://127.0.0.1:8000/ws/cmd")
OUT = Path(os.environ.get("VERIFY_OUT", "/userdata/imx296_accept_148/extra"))
USER = os.environ.get("CAMOS_USER", "admin")
PASS = os.environ.get("CAMOS_PASS", "dlcv2026")
PULSE = Path("/sys/bus/i2c/devices/4-001a/trigger_pulse_us")
RUN_MODE = Path("/sys/bus/i2c/devices/4-001a/run_mode")
N1TRACE = Path("/sys/module/video_rkisp/parameters/n1trace")
TAIL = Path("/sys/module/video_rkisp/parameters/imx296_tail_wait_us")
STATS = Path("/sys/devices/platform/trigger-dev/stats")
UNAME = Path("/proc/version")

det = None
for cand in (
    Path(__file__).resolve().parent / "imx296_bottom_quality.py",
    Path("/userdata/imx296_accept_148/imx296_bottom_quality.py"),
    Path("/userdata/imx296_warmup_ab/scripts/imx296_bottom_quality.py"),
):
    if cand.is_file():
        spec = importlib.util.spec_from_file_location("imx296_bottom_quality", cand)
        det = importlib.util.module_from_spec(spec)
        assert spec.loader
        spec.loader.exec_module(det)
        break
if det is None:
    sys.exit("imx296_bottom_quality.py missing")

OUT.mkdir(parents=True, exist_ok=True)
(OUT / "frames").mkdir(exist_ok=True)

s = requests.Session()


def login():
    s.post(BASE + "/api/auth/login", json={"username": USER, "password": PASS}, timeout=10)
    return "; ".join(f"{c.name}={c.value}" for c in s.cookies)


cookie = login()


def patch(body, timeout=90):
    t0 = time.time()
    r = s.patch(BASE + "/api/flow_config", json=body, timeout=timeout)
    js = r.json() if r.content else {}
    ok = r.status_code < 400
    if isinstance(js, dict) and "success" in js:
        ok = bool(js["success"])
    return time.time() - t0, js, ok


def peek_tag():
    pr = s.get(BASE + "/preview/frame", params={"t": time.time_ns()}, timeout=8)
    return int(pr.headers.get("X-Frame-Tag") or pr.headers.get("x-frame-tag") or 0)


def wait_new(last, timeout_s=2.2):
    end = time.time() + timeout_s
    while time.time() < end:
        pr = s.get(BASE + "/preview/frame", params={"t": time.time_ns()}, timeout=8)
        tn = int(pr.headers.get("X-Frame-Tag") or pr.headers.get("x-frame-tag") or 0)
        if pr.status_code == 200 and pr.content and tn != last:
            return tn, pr.content
        time.sleep(0.03)
    return last, b""


def fire_sw():
    s.post(BASE + "/debug/trigger_once", timeout=8)


def fire_i2c():
    Path("/sys/bus/i2c/devices/4-001a/trigger").write_text("1\n")


def luma_bot_above(a: dict) -> float:
    bot = 0.299 * a["br"] + 0.587 * a["bg"] + 0.114 * a["bb"]
    above = 0.299 * a["ar"] + 0.587 * a["ag"] + 0.114 * a["ab"]
    return bot - above


def olive_whole(a: dict) -> bool:
    # panorama: whole-frame olive mean often (~73,98,45)
    mr, mg, mb = a["mr"], a["mg"], a["mb"]
    g_ex = mg - max(mr, mb)
    return g_ex >= 18 and 50 <= mg <= 130 and mr < 95 and mb < 70


def rkcam():
    return s.get(BASE + "/debug/status", timeout=10).json()["rkcam"]


def parse_stats():
    txt = STATS.read_text()
    out = {}
    for tok in txt.replace("\n", " ").split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            try:
                out[k] = int(v)
            except Exception:
                out[k] = v
    return out


async def ws_cmd(ws, cmd, value=None):
    msg = {"cmd": cmd}
    if value is not None:
        msg["value"] = value
    await ws.send(json.dumps(msg))
    for _ in range(4):
        try:
            await asyncio.wait_for(ws.recv(), timeout=0.3)
        except Exception:
            break


async def set_exp_gain(ws, exp=None, gain=None):
    if gain is not None:
        await ws_cmd(ws, "rkcam_set_gain", float(gain))
    if exp is not None:
        await ws_cmd(ws, "rkcam_set_exposure_time", float(exp))
        # FT 曝光走 PWM pulse；CamOS 应写 trigger_pulse_us。再直写一份兜底。
        try:
            pulse = max(1, int(round(float(exp))) - 14)
            PULSE.write_text(f"{pulse}\n")
        except Exception as e:
            print("pulse_sysfs_err", e, flush=True)
    # wait until rkcam reports (gain always; exp may stay cached)
    deadline = time.time() + 2.5
    while time.time() < deadline:
        st = rkcam()
        g_ok = gain is None or abs(float(st.get("gain") or -1) - float(gain)) < 0.6
        p_ok = True
        if exp is not None and PULSE.exists():
            try:
                p_ok = abs(int(PULSE.read_text().strip()) - (int(round(float(exp))) - 14)) <= 2
            except Exception:
                p_ok = False
        if g_ok and p_ok:
            break
        time.sleep(0.08)
    st = rkcam()
    print(
        f"  set_exp_gain exp={exp} gain={gain} -> rkcam exp={st.get('exposure_us')} "
        f"gain={st.get('gain')} pulse={PULSE.read_text().strip()}",
        flush=True,
    )


def shoot_n(label, n, fire, last=None):
    if last is None:
        last = peek_tag()
    rows = []
    first = None
    for i in range(n):
        fire()
        tn, data = wait_new(last, 2.2)
        if not data:
            rows.append({"i": i + 1, "miss": True})
            continue
        if first is None:
            first = i + 1
        a = det.analyze_jpeg(data)
        dy = luma_bot_above(a)
        rec = {
            "i": i + 1,
            "tag": tn,
            "miss": False,
            "dim": a["dim"],
            "mean": round(a["mean"], 2),
            "dY": round(dy, 2),
            "max_row_jump": a["max_row_jump"],
            "classic": a["classic_green"],
            "seam": a["seam"],
            "tear": a["tear"],
            "bad": a["bad"],
            "olive": olive_whole(a),
            "bot": [a["br"], a["bg"], a["bb"]],
            "above": [a["ar"], a["ag"], a["ab"]],
            "mid": [a["mr"], a["mg"], a["mb"]],
            "reasons": a["reasons"],
        }
        (OUT / "frames" / f"{label}_{i+1:02d}.jpg").write_bytes(data)
        rows.append(rec)
        last = tn
        time.sleep(0.1)
    return {"first_hit": first, "rows": rows, "last": last}


async def main():
    summary = {
        "host": Path("/etc/hostname").read_text().strip() if Path("/etc/hostname").exists() else "",
        "kernel": UNAME.read_text().strip(),
        "uname_v": os.uname().version,
        "n1trace": N1TRACE.read_text().strip() if N1TRACE.exists() else None,
        "tail_wait_us": TAIL.read_text().strip() if TAIL.exists() else None,
        "run_mode_before": RUN_MODE.read_text().strip() if RUN_MODE.exists() else None,
        "pulse_before": PULSE.read_text().strip() if PULSE.exists() else None,
        "stats_before": parse_stats(),
        "rkcam_before": {
            k: rkcam().get(k)
            for k in (
                "exposure_us",
                "gain",
                "trigger_mode",
                "frame_w",
                "frame_h",
                "roi",
                "fps",
                "frame_tag",
                "wb_ratio",
                "last_error",
            )
        },
    }
    print("KERNEL", summary["uname_v"], flush=True)
    print("n1trace", summary["n1trace"], "tail", summary["tail_wait_us"], flush=True)

    orig = dict(summary["rkcam_before"])
    orig_flow = s.get(BASE + "/api/flow_config", timeout=10).json()

    ws = await websockets.connect(
        WS_URL,
        additional_headers={"Cookie": cookie},
        open_timeout=8,
        ping_interval=20,
        max_size=4 * 1024 * 1024,
    )

    # --- enter software ---
    print("=== enter software ===", flush=True)
    dt, body, ok = patch({"run_mode": "trigger", "trigger_mode": "software"})
    print(f"apply_sw={dt:.3f} ok={ok}", flush=True)
    summary["apply_sw_s"] = round(dt, 3)
    summary["apply_sw_ok"] = ok
    time.sleep(0.5)
    await set_exp_gain(ws, gain=240)
    time.sleep(0.3)
    print("rkcam after gain240", rkcam().get("gain"), rkcam().get("exposure_us"), flush=True)

    # 1500 → 15000
    print("=== E 1500 -> 15000 gain=240 ===", flush=True)
    await set_exp_gain(ws, exp=1500, gain=240)
    time.sleep(0.2)
    # discard 2 at low
    shoot_n("e_lo_disc", 2, fire_sw)
    pulse_lo = PULSE.read_text().strip()
    await set_exp_gain(ws, exp=15000, gain=240)
    time.sleep(0.15)
    pulse_hi = PULSE.read_text().strip()
    hi = shoot_n("e_lo2hi", 8, fire_sw)
    dys = [r["dY"] for r in hi["rows"] if not r.get("miss")]
    tears = sum(1 for r in hi["rows"] if r.get("tear"))
    bads = sum(1 for r in hi["rows"] if r.get("bad"))
    olives = sum(1 for r in hi["rows"] if r.get("olive"))
    max_abs_dy = max((abs(x) for x in dys), default=None)
    print(
        f"  lo2hi first={hi['first_hit']} tear={tears} bad={bads} olive={olives} "
        f"max|dY|={max_abs_dy} pulse {pulse_lo}->{pulse_hi}",
        flush=True,
    )
    summary["E_lo2hi"] = {
        "pulse_lo": pulse_lo,
        "pulse_hi": pulse_hi,
        "first_hit": hi["first_hit"],
        "tear": tears,
        "bad": bads,
        "olive": olives,
        "max_abs_dY": max_abs_dy,
        "dYs": dys,
        "rows": hi["rows"],
        "pass": tears == 0 and bads == 0 and olives == 0 and (max_abs_dy is not None and max_abs_dy < 10),
    }

    # 15000 → 1500
    print("=== E 15000 -> 1500 gain=240 ===", flush=True)
    await set_exp_gain(ws, exp=15000, gain=240)
    shoot_n("e_hi_disc", 2, fire_sw)
    await set_exp_gain(ws, exp=1500, gain=240)
    time.sleep(0.15)
    lo = shoot_n("e_hi2lo", 8, fire_sw)
    dys2 = [r["dY"] for r in lo["rows"] if not r.get("miss")]
    tears2 = sum(1 for r in lo["rows"] if r.get("tear"))
    bads2 = sum(1 for r in lo["rows"] if r.get("bad"))
    olives2 = sum(1 for r in lo["rows"] if r.get("olive"))
    max_abs_dy2 = max((abs(x) for x in dys2), default=None)
    pulse_after = PULSE.read_text().strip()
    print(
        f"  hi2lo first={lo['first_hit']} tear={tears2} bad={bads2} olive={olives2} "
        f"max|dY|={max_abs_dy2} pulse_after={pulse_after}",
        flush=True,
    )
    summary["E_hi2lo"] = {
        "pulse_after": pulse_after,
        "first_hit": lo["first_hit"],
        "tear": tears2,
        "bad": bads2,
        "olive": olives2,
        "max_abs_dY": max_abs_dy2,
        "dYs": dys2,
        "rows": lo["rows"],
        "pass": tears2 == 0 and bads2 == 0 and olives2 == 0 and (max_abs_dy2 is not None and max_abs_dy2 < 10),
    }

    # I: after short exp, pulse should track current (not stuck at short)
    await set_exp_gain(ws, exp=15000, gain=0)
    time.sleep(0.5)
    pulse_restore = PULSE.read_text().strip()
    try:
        pv = int(pulse_restore)
    except Exception:
        pv = -1
    summary["I_pulse_restore"] = {
        "pulse": pulse_restore,
        "pass": pv > 10000,  # 15000-14 ≈ 14986
    }
    print(f"I pulse after 15000={pulse_restore} pass={summary['I_pulse_restore']['pass']}", flush=True)

    # restore milder gain for remaining
    await set_exp_gain(ws, exp=orig.get("exposure_us") or 3000, gain=0)

    # --- F: mon → line0 idle, no fire, tag must not +1 ---
    print("=== F idle line0 no-fire ===", flush=True)
    dtm, _, okm = patch({"run_mode": "monitor", "trigger_mode": "close"})
    time.sleep(5.5)
    tag0 = peek_tag()
    stats0 = parse_stats()
    rk0 = {
        k: rkcam().get(k)
        for k in ("frame_tag", "frame_seq", "native_frame_seq", "preview_seq")
    }
    dtl, _, okl = patch({"run_mode": "trigger", "trigger_mode": "line0"})
    time.sleep(2.5)
    tag1 = peek_tag()
    stats1 = parse_stats()
    rk1 = {
        k: rkcam().get(k)
        for k in ("frame_tag", "frame_seq", "native_frame_seq", "preview_seq")
    }
    idle_delta = (tag1 or 0) - (tag0 or 0)
    irq_delta = (stats1.get("irq") or 0) - (stats0.get("irq") or 0)
    # /preview/frame 的 X-Frame-Tag 来自 encoded hold-last-frame（切换后仍显示
    # 最后一张 monitor JPEG 的 raw_seq）。用户路径以 raw/frame_seq 为准：
    # quarantine 后应为 0，且无 echo 时不得再 put。
    user_seq = int(rk1.get("frame_seq") or rk1.get("frame_tag") or 0)
    print(
        f"  apply_l0={dtl:.3f} ok={okl} preview_tag {tag0}->{tag1} delta={idle_delta} "
        f"irq_delta={irq_delta} user_seq={user_seq} rk {rk0}->{rk1}",
        flush=True,
    )
    # 再等 2s：warmup 可能刚好吃掉 1 个 tag；稳态不应继续涨
    time.sleep(2.0)
    tag2 = peek_tag()
    late_delta = (tag2 or 0) - (tag1 or 0)
    rk2 = {
        k: rkcam().get(k)
        for k in ("frame_tag", "frame_seq", "native_frame_seq", "preview_seq")
    }
    user_seq2 = int(rk2.get("frame_seq") or rk2.get("frame_tag") or 0)
    print(
        f"  idle+2s preview_tag {tag1}->{tag2} late_delta={late_delta} "
        f"user_seq {user_seq}->{user_seq2}",
        flush=True,
    )
    summary["F_idle_line0"] = {
        "apply_s": round(dtl, 3),
        "apply_ok": okl,
        "tag0": tag0,
        "tag1": tag1,
        "tag2": tag2,
        "tag_delta": idle_delta,
        "late_delta": late_delta,
        "irq_delta": irq_delta,
        "rk0": rk0,
        "rk1": rk1,
        "rk2": rk2,
        "user_seq": user_seq,
        "user_seq2": user_seq2,
        "stats0": stats0,
        "stats1": stats1,
        # preview tag 在 apply 期间可能被 monitor hold 编码推高，不等于入模。
        "enter_spurious": user_seq != 0 and irq_delta == 0,
        "pass": okl and late_delta == 0 and user_seq == 0 and user_seq2 == 0,
    }

    # first i2c fire after idle (mtime fallback, not true GPIO)
    last = peek_tag()
    i2c = shoot_n("f_i2c", 6, fire_i2c, last=last)
    stats2 = parse_stats()
    olives_i = sum(1 for r in i2c["rows"] if r.get("olive"))
    bads_i = sum(1 for r in i2c["rows"] if r.get("bad"))
    print(
        f"  i2c first={i2c['first_hit']} got={sum(1 for r in i2c['rows'] if not r.get('miss'))} "
        f"bad={bads_i} olive={olives_i} irq {stats1.get('irq')}->{stats2.get('irq')}",
        flush=True,
    )
    summary["F_i2c_line0"] = {
        "first_hit": i2c["first_hit"],
        "got": sum(1 for r in i2c["rows"] if not r.get("miss")),
        "bad": bads_i,
        "olive": olives_i,
        "irq_before": stats1.get("irq"),
        "irq_after": stats2.get("irq"),
        "note": "i2c echo does not increment trigger-dev irq; mtime fallback",
        "rows": i2c["rows"],
        "pass": i2c["first_hit"] == 1 and bads_i == 0 and olives_i == 0,
    }

    # J
    summary["J_n1trace_off"] = {
        "value": N1TRACE.read_text().strip(),
        "pass": N1TRACE.read_text().strip() in ("N", "0", "n"),
    }

    # restore monitor + original exp/gain
    print("=== restore ===", flush=True)
    patch({"run_mode": orig_flow.get("run_mode", "monitor"), "trigger_mode": orig_flow.get("trigger_mode", "close")})
    await set_exp_gain(ws, exp=orig.get("exposure_us"), gain=orig.get("gain"))
    await ws.close()

    summary["E_pass"] = bool(summary["E_lo2hi"]["pass"] and summary["E_hi2lo"]["pass"])
    summary["F_idle_pass"] = bool(summary["F_idle_line0"]["pass"])
    summary["extra_pass"] = bool(
        summary["E_pass"]
        and summary["F_idle_pass"]
        and summary["I_pulse_restore"]["pass"]
        and summary["J_n1trace_off"]["pass"]
    )
    (OUT / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print("BEGIN_EXTRA_JSON", flush=True)
    slim = {k: summary[k] for k in summary if k not in ("E_lo2hi", "E_hi2lo", "F_i2c_line0", "F_idle_line0")}
    slim["E_lo2hi"] = {k: summary["E_lo2hi"][k] for k in summary["E_lo2hi"] if k != "rows"}
    slim["E_hi2lo"] = {k: summary["E_hi2lo"][k] for k in summary["E_hi2lo"] if k != "rows"}
    slim["F_idle_line0"] = {k: summary["F_idle_line0"][k] for k in summary["F_idle_line0"] if k not in ("stats0", "stats1")}
    slim["F_i2c_line0"] = {k: summary["F_i2c_line0"][k] for k in summary["F_i2c_line0"] if k != "rows"}
    print(json.dumps(slim, ensure_ascii=False), flush=True)
    print("END_EXTRA_JSON", flush=True)
    print("EXTRA", "PASS" if summary["extra_pass"] else "FAIL", flush=True)


if __name__ == "__main__":
    asyncio.run(main())
