#!/usr/bin/env python3
"""#1301 latency + multi-signal quality matrix.

Measures mon→software apply wall-clock (real switches only: apply_s>=0.5).
Hard limit 5s target 2–3s. Does not lengthen warmup/gate.
"""
from __future__ import annotations

import asyncio
import importlib.util
import json
import os
import statistics
import sys
import time
from pathlib import Path

import requests
import websockets

BASE = os.environ.get("CAMOS_BASE", "http://127.0.0.1:8000")
WS_URL = os.environ.get("CAMOS_WS", "ws://127.0.0.1:8000/ws")
OUT = Path(os.environ.get("VERIFY_OUT", "/userdata/imx296_warmup_ab/verify_k1301_lat"))
SW_ROUNDS = int(os.environ.get("VERIFY_SW_ROUNDS", "8"))
HEAD = int(os.environ.get("VERIFY_HEAD", "10"))
ROI_HEAD = int(os.environ.get("VERIFY_ROI_HEAD", "8"))
COOLDOWN = float(os.environ.get("VERIFY_COOLDOWN", "5.5"))
USER, PASS = "admin", "dlcv2026"

_det = None
for cand in (
    Path(__file__).resolve().parent / "imx296_bottom_quality.py",
    Path("/userdata/imx296_warmup_ab/scripts/imx296_bottom_quality.py"),
):
    if cand.is_file():
        spec = importlib.util.spec_from_file_location("imx296_bottom_quality", cand)
        _det = importlib.util.module_from_spec(spec)
        assert spec.loader
        spec.loader.exec_module(_det)
        break
if _det is None:
    sys.exit("imx296_bottom_quality.py missing")

if OUT.exists():
    import shutil

    shutil.rmtree(OUT)
OUT.mkdir(parents=True)
(OUT / "frames").mkdir()
(OUT / "bad").mkdir()

s = requests.Session()


def login():
    s.post(BASE + "/api/auth/login", json={"username": USER, "password": PASS}, timeout=10)
    return "; ".join(f"{c.name}={c.value}" for c in s.cookies)


cookie = login()


def patch(body, timeout=90):
    t0 = time.time()
    try:
        r = s.patch(BASE + "/api/flow_config", json=body, timeout=timeout)
        js = r.json() if r.content else {}
        ok = r.status_code < 400
        if isinstance(js, dict):
            if "success" in js:
                ok = bool(js["success"])
            elif "ok" in js:
                ok = bool(js["ok"])
        return time.time() - t0, js, None, ok
    except Exception as e:
        return time.time() - t0, {}, str(e), False


def peek_tag():
    try:
        pr = s.get(BASE + "/preview/frame", params={"t": time.time_ns()}, timeout=8)
        return int(pr.headers.get("x-frame-tag") or pr.headers.get("X-Frame-Tag") or 0)
    except Exception:
        return 0


def shoot(label, n, mode="software"):
    last = peek_tag()
    first = None
    stats = {
        "got": 0,
        "bad": 0,
        "classic": 0,
        "seam": 0,
        "tear": 0,
        "idx": [],
        "bots": [],
        "dims": [],
    }
    for i in range(n):
        try:
            if mode == "line0":
                Path("/sys/bus/i2c/devices/4-001a/trigger").write_text("1\n")
            else:
                s.post(BASE + "/debug/trigger_once", timeout=8)
        except Exception:
            pass
        end = time.time() + 2.0
        hit = False
        data = b""
        while time.time() < end:
            try:
                pr = s.get(BASE + "/preview/frame", params={"t": time.time_ns()}, timeout=8)
            except Exception:
                time.sleep(0.04)
                continue
            h = pr.headers.get("x-frame-tag") or pr.headers.get("X-Frame-Tag") or ""
            try:
                tn = int(h) if str(h).isdigit() else -1
            except Exception:
                tn = -1
            if pr.status_code == 200 and pr.content and tn != last and tn >= 0:
                hit = True
                last = tn
                data = pr.content
                break
            time.sleep(0.03)
        if first is None and hit:
            first = i + 1
        if not hit:
            continue
        stats["got"] += 1
        (OUT / "frames" / f"{label}_s{i+1:02d}.jpg").write_bytes(data)
        a = _det.analyze_jpeg(data)
        stats["dims"].append(a.get("dim"))
        if len(stats["bots"]) < 3:
            stats["bots"].append({"i": i + 1, **a})
        if a.get("green") or a.get("bad"):
            stats["bad"] += 1
            stats["classic"] += int(bool(a.get("classic_green")))
            stats["seam"] += int(bool(a.get("seam")))
            stats["tear"] += int(bool(a.get("tear")))
            stats["idx"].append(i + 1)
            (OUT / "bad" / f"{label}_s{i+1:02d}.jpg").write_bytes(data)
            (OUT / "bad" / f"{label}_s{i+1:02d}.json").write_text(
                json.dumps(a, ensure_ascii=False, indent=2), encoding="utf-8"
            )
        time.sleep(0.1)
    stats["first_hit"] = first
    return stats


async def ws_cmd(ws, cmd, value=None):
    msg = {"cmd": cmd}
    if value is not None:
        msg["value"] = value
    await ws.send(json.dumps(msg))
    for _ in range(3):
        try:
            await asyncio.wait_for(ws.recv(), timeout=0.25)
        except Exception:
            break


def lat_stats(vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return {"n": 0}
    return {
        "n": len(vals),
        "min": round(min(vals), 3),
        "max": round(max(vals), 3),
        "avg": round(sum(vals) / len(vals), 3),
        "p50": round(statistics.median(vals), 3),
        "all": [round(v, 3) for v in vals],
    }


async def main():
    summary = {
        "kernel": Path("/proc/version").read_text().strip(),
        "detector": "imx296_bottom_quality",
        "sw": [],
        "roi": [],
        "targets": {"apply_hard_s": 5.0, "apply_goal_s": [2.0, 3.0]},
    }

    # ensure mon
    dt, _, err, ok = patch({"run_mode": "monitor", "trigger_mode": "none"})
    print(f"init mon dt={dt:.3f} ok={ok} err={err}", flush=True)
    time.sleep(COOLDOWN)

    real_applies = []
    for r in range(SW_ROUNDS):
        print(f"=== SW R{r} mon->software ===", flush=True)
        # force mon first if previous left us in trigger
        patch({"run_mode": "monitor", "trigger_mode": "none"})
        time.sleep(COOLDOWN)
        dt, body, err, ok = patch({"run_mode": "trigger", "trigger_mode": "software"}, timeout=90)
        real = bool(ok) and dt >= 0.5 and err is None
        if real:
            real_applies.append(dt)
        print(
            f"  apply={dt:.3f}s ok={ok} real_switch={real} err={err}",
            flush=True,
        )
        time.sleep(0.35)
        sh = shoot(f"sw_r{r}", HEAD)
        print(
            f"  first={sh['first_hit']} got={sh['got']} bad={sh['bad']} "
            f"c/s/t={sh['classic']}/{sh['seam']}/{sh['tear']} idx={sh['idx']}",
            flush=True,
        )
        if sh["bots"]:
            b = sh["bots"][0]
            print(
                f"  head bot=({b.get('br')},{b.get('bg')},{b.get('bb')}) "
                f"above=({b.get('ar')},{b.get('ag')},{b.get('ab')}) "
                f"reasons={b.get('reasons')}",
                flush=True,
            )
        summary["sw"].append(
            {
                "r": r,
                "apply_s": round(dt, 3),
                "apply_ok": ok,
                "real_switch": real,
                "apply_err": err,
                **{k: sh[k] for k in ("first_hit", "got", "bad", "classic", "seam", "tear", "idx")},
                "bots_head": sh["bots"][:2],
            }
        )
        dt2, _, err2, _ = patch({"run_mode": "monitor", "trigger_mode": "none"})
        print(f"  back mon dt={dt2:.3f} err={err2}", flush=True)
        time.sleep(0.3)

    # ROI under software
    print("=== ROI WS ===", flush=True)
    dt, _, err, ok = patch({"run_mode": "trigger", "trigger_mode": "software"})
    print(f"enter sw for ROI apply={dt:.3f} ok={ok} err={err}", flush=True)
    time.sleep(COOLDOWN)
    cookie2 = login()
    ws = await websockets.connect(
        WS_URL,
        additional_headers={"Cookie": cookie2},
        open_timeout=8,
        ping_interval=20,
        max_size=4 * 1024 * 1024,
    )
    steps = [
        ("base", None),
        ("roi640", {"x": 100, "y": 100, "w": 640, "h": 480}),
        ("roi800", {"x": 200, "y": 150, "w": 800, "h": 600}),
        ("reset", "reset"),
        ("roi512", {"x": 50, "y": 80, "w": 512, "h": 384}),
        ("reset2", "reset"),
    ]
    for name, roi in steps:
        t0 = time.time()
        try:
            if roi == "reset":
                await ws_cmd(ws, "rkcam_reset_roi")
            elif isinstance(roi, dict):
                await ws_cmd(ws, "rkcam_set_roi", roi)
        except Exception as e:
            print(f"  {name} ws_err={e}", flush=True)
        roi_dt = time.time() - t0
        time.sleep(4.0)
        try:
            sh = shoot(f"roi_{name}", ROI_HEAD)
        except Exception as e:
            print(f"  {name} shoot_err={e}", flush=True)
            login()
            summary["roi"].append({"name": name, "error": str(e), "bad": -1})
            continue
        print(
            f"  {name} roi_cmd={roi_dt:.3f}s dim={sh['dims'][:1]} first={sh['first_hit']} "
            f"got={sh['got']} bad={sh['bad']} c/s/t={sh['classic']}/{sh['seam']}/{sh['tear']}",
            flush=True,
        )
        summary["roi"].append(
            {
                "name": name,
                "roi": roi,
                "roi_cmd_s": round(roi_dt, 3),
                **{k: sh[k] for k in ("first_hit", "got", "bad", "classic", "seam", "tear", "idx", "dims")},
                "bots_head": sh["bots"][:2],
            }
        )
    await ws.close()
    patch({"run_mode": "monitor", "trigger_mode": "none"})

    bad_sw = sum(x.get("bad") or 0 for x in summary["sw"] if (x.get("bad") or 0) >= 0)
    bad_roi = sum(x.get("bad") or 0 for x in summary["roi"] if (x.get("bad") or 0) >= 0)
    firsts = [
        x.get("first_hit")
        for x in summary["sw"] + summary["roi"]
        if x.get("first_hit") is not None
    ]
    lat = lat_stats(real_applies)
    within_3 = all(v <= 3.0 for v in real_applies) if real_applies else False
    within_5 = all(v <= 5.0 for v in real_applies) if real_applies else False
    summary["totals"] = {
        "bad_sw": bad_sw,
        "bad_roi": bad_roi,
        "bad_total": bad_sw + bad_roi,
        "classic": sum(x.get("classic") or 0 for x in summary["sw"] + summary["roi"]),
        "seam": sum(x.get("seam") or 0 for x in summary["sw"] + summary["roi"]),
        "tear": sum(x.get("tear") or 0 for x in summary["sw"] + summary["roi"]),
        "first_all_1": all(f == 1 for f in firsts) if firsts else False,
        "apply_latency": lat,
        "apply_all_le_3s": within_3,
        "apply_all_le_5s": within_5,
        "quality_pass": (bad_sw + bad_roi) == 0
        and (all(f == 1 for f in firsts) if firsts else False),
        "latency_pass_hard": within_5 and bool(real_applies),
        "latency_pass_goal": within_3 and bool(real_applies),
        "dim_changed": any(
            (x.get("dims") or [None])[0] not in (None, "1456x1088")
            for x in summary["roi"]
            if str(x.get("name", "")).startswith("roi") and not str(x.get("name")).startswith("reset")
        ),
    }
    (OUT / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print("BEGIN_SUMMARY_JSON", flush=True)
    print(json.dumps(summary["totals"], ensure_ascii=False), flush=True)
    print("END_SUMMARY_JSON", flush=True)
    print("OUT", OUT, flush=True)


if __name__ == "__main__":
    asyncio.run(main())
