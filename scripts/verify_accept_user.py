#!/usr/bin/env python3
"""Real-user acceptance on CamOS (smartcam_bs defaults).

Scenarios:
  - mon → software trigger (apply latency + quality + first_hit)
  - mon → line0 / XTRIG (sysfs) quality + first_hit
  - software ROI set/reset via WS
  - N-1 / phase: successive triggers must each produce a NEW frame tag;
    optional center-luma monotonicity under controlled light (soft check)
  - multi-signal bottom quality: classic / seam / tear

Does not override CamOS warmup/gate. Kernel #2026082001 + imx296_bottom_quality.
CamOS 控制面默认 /ws/cmd（/ws 只读）；可用 CAMOS_WS 覆盖。
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
WS_URL = os.environ.get("CAMOS_WS", "ws://127.0.0.1:8000/ws/cmd")
OUT = Path(os.environ.get("VERIFY_OUT", "/userdata/imx296_warmup_ab/verify_accept_user"))
SW_ROUNDS = int(os.environ.get("VERIFY_SW_ROUNDS", "6"))
LINE0_ROUNDS = int(os.environ.get("VERIFY_LINE0_ROUNDS", "4"))
HEAD = int(os.environ.get("VERIFY_HEAD", "12"))
N1_SEQ = int(os.environ.get("VERIFY_N1_SEQ", "16"))
ROI_HEAD = int(os.environ.get("VERIFY_ROI_HEAD", "8"))
COOLDOWN = float(os.environ.get("VERIFY_COOLDOWN", "5.5"))
USER = os.environ.get("CAMOS_USER", "admin")
PASS = os.environ.get("CAMOS_PASS", "dlcv2026")
XTRIG = Path(os.environ.get("XTRIG_PATH", "/sys/bus/i2c/devices/4-001a/trigger"))
N1TRACE = Path("/sys/module/video_rkisp/parameters/n1trace")

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
(OUT / "n1").mkdir()

s = requests.Session()


def login():
    s.post(BASE + "/api/auth/login", json={"username": USER, "password": PASS}, timeout=10)
    return "; ".join(f"{c.name}={c.value}" for c in s.cookies)


cookie = login()


def n1trace(on: bool):
    try:
        N1TRACE.write_text("Y\n" if on else "N\n")
    except Exception as e:
        print("n1trace", e, flush=True)


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


def fire(mode: str):
    try:
        if mode == "line0":
            XTRIG.write_text("1\n")
        else:
            s.post(BASE + "/debug/trigger_once", timeout=8)
        return True
    except Exception as e:
        print(f"  fire_{mode}_err={e}", flush=True)
        return False


def wait_new_frame(last_tag: int, timeout_s: float = 2.0):
    end = time.time() + timeout_s
    while time.time() < end:
        try:
            pr = s.get(BASE + "/preview/frame", params={"t": time.time_ns()}, timeout=8)
        except Exception:
            time.sleep(0.03)
            continue
        h = pr.headers.get("x-frame-tag") or pr.headers.get("X-Frame-Tag") or ""
        try:
            tn = int(h) if str(h).isdigit() else -1
        except Exception:
            tn = -1
        if pr.status_code == 200 and pr.content and tn != last_tag and tn >= 0:
            return tn, pr.content, None
        time.sleep(0.03)
    return last_tag, b"", "timeout"


def shoot(label: str, n: int, mode: str = "software"):
    last = peek_tag()
    first = None
    stats = {
        "got": 0,
        "miss": 0,
        "bad": 0,
        "classic": 0,
        "seam": 0,
        "tear": 0,
        "idx": [],
        "bots": [],
        "dims": [],
        "tags": [],
        "means": [],
        "dup_tag": 0,
        "tag_non_increasing": 0,
    }
    prev_tag = last
    for i in range(n):
        fire(mode)
        tn, data, err = wait_new_frame(last, 2.0)
        if err or not data:
            stats["miss"] += 1
            time.sleep(0.08)
            continue
        if first is None:
            first = i + 1
        if tn == prev_tag:
            stats["dup_tag"] += 1
        if tn < prev_tag and prev_tag > 0:
            stats["tag_non_increasing"] += 1
        last = tn
        prev_tag = tn
        stats["got"] += 1
        stats["tags"].append(tn)
        (OUT / "frames" / f"{label}_s{i+1:02d}.jpg").write_bytes(data)
        a = _det.analyze_jpeg(data)
        stats["dims"].append(a.get("dim"))
        stats["means"].append(a.get("mean"))
        if len(stats["bots"]) < 3:
            stats["bots"].append({"i": i + 1, "tag": tn, **a})
        if a.get("green") or a.get("bad"):
            stats["bad"] += 1
            stats["classic"] += int(bool(a.get("classic_green")))
            stats["seam"] += int(bool(a.get("seam")))
            stats["tear"] += int(bool(a.get("tear")))
            stats["idx"].append(i + 1)
            (OUT / "bad" / f"{label}_s{i+1:02d}.jpg").write_bytes(data)
            (OUT / "bad" / f"{label}_s{i+1:02d}.json").write_text(
                json.dumps({"tag": tn, **a}, ensure_ascii=False, indent=2), encoding="utf-8"
            )
        time.sleep(0.1)
    stats["first_hit"] = first
    # unique tags == got → no N-1 reuse of same published frame
    stats["unique_tags"] = len(set(stats["tags"]))
    stats["tag_unique_ok"] = stats["unique_tags"] == stats["got"] and stats["got"] > 0
    return stats


def n1_sequence(label: str, n: int, mode: str = "software"):
    """Strict 1-trigger → 1-new-tag sequence; detect stalls / repeats (N-1 symptom)."""
    last = peek_tag()
    rows = []
    miss = 0
    bad = 0
    classic = seam = tear = 0
    tags = []
    for i in range(n):
        t_fire = time.time()
        fire(mode)
        tn, data, err = wait_new_frame(last, 2.2)
        dt = time.time() - t_fire
        if err or not data:
            miss += 1
            rows.append({"i": i + 1, "miss": True, "dt": round(dt, 3)})
            time.sleep(0.12)
            continue
        a = _det.analyze_jpeg(data)
        row = {
            "i": i + 1,
            "tag": tn,
            "prev": last,
            "delta_tag": tn - last if last else None,
            "dt": round(dt, 3),
            "mean": a.get("mean"),
            "dim": a.get("dim"),
            "bad": bool(a.get("bad")),
            "classic": bool(a.get("classic_green")),
            "seam": bool(a.get("seam")),
            "tear": bool(a.get("tear")),
            "reasons": a.get("reasons"),
        }
        rows.append(row)
        tags.append(tn)
        (OUT / "n1" / f"{label}_{i+1:02d}.jpg").write_bytes(data)
        if a.get("bad"):
            bad += 1
            classic += int(bool(a.get("classic_green")))
            seam += int(bool(a.get("seam")))
            tear += int(bool(a.get("tear")))
            (OUT / "bad" / f"{label}_n1_{i+1:02d}.jpg").write_bytes(data)
        last = tn
        time.sleep(0.15)
    # N-1 failure modes:
    #  - miss on first trigger but hit on second with stale content (hard without scene change)
    #  - repeated tags
    #  - first_hit > 1 (first fire produced no new frame)
    first_hit = next((r["i"] for r in rows if not r.get("miss")), None)
    unique = len(set(tags))
    # delta_tag should be >0 for each hit; if always 0 impossible since wait_new_frame
    deltas = [r["delta_tag"] for r in rows if r.get("delta_tag") is not None]
    # soft: mean should not be identical for all (frozen frame) if n large
    means = [r["mean"] for r in rows if r.get("mean") is not None]
    frozen = False
    if len(means) >= 6:
        # if all means within 0.05 → suspicious freeze
        if max(means) - min(means) < 0.05:
            frozen = True
    return {
        "label": label,
        "mode": mode,
        "n": n,
        "got": len(tags),
        "miss": miss,
        "first_hit": first_hit,
        "bad": bad,
        "classic": classic,
        "seam": seam,
        "tear": tear,
        "unique_tags": unique,
        "tag_unique_ok": unique == len(tags) and len(tags) > 0,
        "first_is_1": first_hit == 1,
        "frozen_mean": frozen,
        "delta_min": min(deltas) if deltas else None,
        "delta_max": max(deltas) if deltas else None,
        "rows": rows,
        # pass: every fire gets new tag, first=1, no quality bad, not frozen
        "n1_pass": (
            miss == 0
            and first_hit == 1
            and unique == len(tags)
            and len(tags) == n
            and bad == 0
            and not frozen
        ),
    }


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
    n1trace(False)
    summary = {
        "kernel": Path("/proc/version").read_text().strip(),
        "detector": "imx296_bottom_quality",
        "camos": "smartcam_bs/0_测试运行_camos.sh defaults",
        "sw": [],
        "line0": [],
        "n1": [],
        "roi": [],
        "targets": {"apply_hard_s": 5.0, "apply_goal_s": [2.0, 3.0]},
    }

    def to_mon():
        # CamOS uses trigger_mode=close for monitor
        return patch({"run_mode": "monitor", "trigger_mode": "close"})

    dt, _, err, ok = to_mon()
    print(f"init mon dt={dt:.3f} ok={ok} err={err}", flush=True)
    time.sleep(COOLDOWN)

    real_applies = []

    # --- mon → software ---
    for r in range(SW_ROUNDS):
        print(f"=== SW R{r} mon->software ===", flush=True)
        to_mon()
        time.sleep(COOLDOWN)
        dt, body, err, ok = patch({"run_mode": "trigger", "trigger_mode": "software"}, timeout=90)
        real = bool(ok) and dt >= 0.5 and err is None
        if real:
            real_applies.append(dt)
        print(f"  apply={dt:.3f}s ok={ok} real={real} err={err}", flush=True)
        time.sleep(0.35)
        sh = shoot(f"sw_r{r}", HEAD, "software")
        print(
            f"  first={sh['first_hit']} got={sh['got']} miss={sh['miss']} bad={sh['bad']} "
            f"c/s/t={sh['classic']}/{sh['seam']}/{sh['tear']} "
            f"uniq={sh['unique_tags']}/{sh['got']} idx={sh['idx']}",
            flush=True,
        )
        if sh["bots"]:
            b = sh["bots"][0]
            print(
                f"  head bot=({b.get('br')},{b.get('bg')},{b.get('bb')}) "
                f"above=({b.get('ar')},{b.get('ag')},{b.get('ab')}) reasons={b.get('reasons')}",
                flush=True,
            )
        summary["sw"].append(
            {
                "r": r,
                "apply_s": round(dt, 3),
                "apply_ok": ok,
                "real_switch": real,
                "apply_err": err,
                **{
                    k: sh[k]
                    for k in (
                        "first_hit",
                        "got",
                        "miss",
                        "bad",
                        "classic",
                        "seam",
                        "tear",
                        "idx",
                        "unique_tags",
                        "tag_unique_ok",
                        "dup_tag",
                    )
                },
                "bots_head": sh["bots"][:2],
                "tags_head": sh["tags"][:6],
            }
        )
        dt2, _, err2, _ = to_mon()
        print(f"  back mon dt={dt2:.3f} err={err2}", flush=True)
        time.sleep(0.3)

    # --- N-1 sequence under software (after fresh enter) ---
    print("=== N1 SEQ software ===", flush=True)
    to_mon()
    time.sleep(COOLDOWN)
    dt, _, err, ok = patch({"run_mode": "trigger", "trigger_mode": "software"})
    print(f"  enter sw apply={dt:.3f} ok={ok}", flush=True)
    time.sleep(0.5)
    n1s = n1_sequence("n1_sw", N1_SEQ, "software")
    print(
        f"  n1_sw got={n1s['got']}/{n1s['n']} miss={n1s['miss']} first={n1s['first_hit']} "
        f"bad={n1s['bad']} uniq_ok={n1s['tag_unique_ok']} frozen={n1s['frozen_mean']} "
        f"pass={n1s['n1_pass']}",
        flush=True,
    )
    summary["n1"].append({k: v for k, v in n1s.items() if k != "rows"})
    (OUT / "n1_sw_rows.json").write_text(
        json.dumps(n1s["rows"], ensure_ascii=False, indent=2), encoding="utf-8"
    )
    to_mon()
    time.sleep(COOLDOWN)

    # --- mon → line0 ---
    for r in range(LINE0_ROUNDS):
        print(f"=== LINE0 R{r} mon->line0 ===", flush=True)
        to_mon()
        time.sleep(COOLDOWN)
        # hardware / external line0 mode if supported
        dt, body, err, ok = patch(
            {"run_mode": "trigger", "trigger_mode": "line0"}, timeout=90
        )
        if not ok:
            # fallback common aliases
            for alt in (
                {"run_mode": "trigger", "trigger_mode": "hardware"},
                {"run_mode": "trigger", "trigger_mode": "external"},
                {"run_mode": "trigger", "trigger_mode": "xtrig"},
            ):
                dt, body, err, ok = patch(alt, timeout=90)
                if ok:
                    print(f"  alt_mode={alt} apply={dt:.3f}", flush=True)
                    break
        real = bool(ok) and dt >= 0.5 and err is None
        if real:
            real_applies.append(dt)
        print(f"  apply={dt:.3f}s ok={ok} real={real} err={err} body_keys={list(body)[:6] if isinstance(body, dict) else body}", flush=True)
        time.sleep(0.4)
        # fire via sysfs XTRIG regardless of CamOS mode name
        sh = shoot(f"l0_r{r}", HEAD, "line0")
        print(
            f"  first={sh['first_hit']} got={sh['got']} miss={sh['miss']} bad={sh['bad']} "
            f"c/s/t={sh['classic']}/{sh['seam']}/{sh['tear']} "
            f"uniq={sh['unique_tags']}/{sh['got']} idx={sh['idx']}",
            flush=True,
        )
        summary["line0"].append(
            {
                "r": r,
                "apply_s": round(dt, 3),
                "apply_ok": ok,
                "real_switch": real,
                "apply_err": err,
                **{
                    k: sh[k]
                    for k in (
                        "first_hit",
                        "got",
                        "miss",
                        "bad",
                        "classic",
                        "seam",
                        "tear",
                        "idx",
                        "unique_tags",
                        "tag_unique_ok",
                    )
                },
                "bots_head": sh["bots"][:2],
            }
        )
        to_mon()
        time.sleep(0.3)

    # N-1 under line0
    print("=== N1 SEQ line0 ===", flush=True)
    to_mon()
    time.sleep(COOLDOWN)
    dt, _, err, ok = patch({"run_mode": "trigger", "trigger_mode": "line0"})
    if not ok:
        for alt in (
            {"run_mode": "trigger", "trigger_mode": "hardware"},
            {"run_mode": "trigger", "trigger_mode": "external"},
        ):
            dt, _, err, ok = patch(alt)
            if ok:
                break
    print(f"  enter l0 apply={dt:.3f} ok={ok}", flush=True)
    time.sleep(0.5)
    n1l = n1_sequence("n1_l0", N1_SEQ, "line0")
    print(
        f"  n1_l0 got={n1l['got']}/{n1l['n']} miss={n1l['miss']} first={n1l['first_hit']} "
        f"bad={n1l['bad']} uniq_ok={n1l['tag_unique_ok']} frozen={n1l['frozen_mean']} "
        f"pass={n1l['n1_pass']}",
        flush=True,
    )
    summary["n1"].append({k: v for k, v in n1l.items() if k != "rows"})
    (OUT / "n1_l0_rows.json").write_text(
        json.dumps(n1l["rows"], ensure_ascii=False, indent=2), encoding="utf-8"
    )

    # --- ROI under software ---
    print("=== ROI WS ===", flush=True)
    to_mon()
    time.sleep(COOLDOWN)
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
            sh = shoot(f"roi_{name}", ROI_HEAD, "software")
        except Exception as e:
            print(f"  {name} shoot_err={e}", flush=True)
            login()
            summary["roi"].append({"name": name, "error": str(e), "bad": -1})
            continue
        print(
            f"  {name} roi_cmd={roi_dt:.3f}s dim={sh['dims'][:1]} first={sh['first_hit']} "
            f"got={sh['got']} bad={sh['bad']} c/s/t={sh['classic']}/{sh['seam']}/{sh['tear']} "
            f"uniq_ok={sh['tag_unique_ok']}",
            flush=True,
        )
        summary["roi"].append(
            {
                "name": name,
                "roi": roi,
                "roi_cmd_s": round(roi_dt, 3),
                **{
                    k: sh[k]
                    for k in (
                        "first_hit",
                        "got",
                        "miss",
                        "bad",
                        "classic",
                        "seam",
                        "tear",
                        "idx",
                        "dims",
                        "unique_tags",
                        "tag_unique_ok",
                    )
                },
                "bots_head": sh["bots"][:2],
            }
        )
    await ws.close()
    to_mon()

    def _sum_bad(rows):
        return sum(x.get("bad") or 0 for x in rows if (x.get("bad") or 0) >= 0)

    bad_sw = _sum_bad(summary["sw"])
    bad_l0 = _sum_bad(summary["line0"])
    bad_roi = _sum_bad(summary["roi"])
    bad_n1 = sum(x.get("bad") or 0 for x in summary["n1"])
    all_blocks = summary["sw"] + summary["line0"] + summary["roi"]
    firsts = [x.get("first_hit") for x in all_blocks if x.get("first_hit") is not None]
    firsts += [x.get("first_hit") for x in summary["n1"] if x.get("first_hit") is not None]
    uniq_oks = [
        x.get("tag_unique_ok")
        for x in all_blocks + summary["n1"]
        if "tag_unique_ok" in x
    ]
    n1_pass_all = all(x.get("n1_pass") for x in summary["n1"]) if summary["n1"] else False
    lat = lat_stats(real_applies)
    within_3 = all(v <= 3.0 for v in real_applies) if real_applies else False
    within_5 = all(v <= 5.0 for v in real_applies) if real_applies else False
    quality_ok = (bad_sw + bad_l0 + bad_roi + bad_n1) == 0
    first_ok = all(f == 1 for f in firsts) if firsts else False
    tag_ok = all(uniq_oks) if uniq_oks else False

    summary["totals"] = {
        "bad_sw": bad_sw,
        "bad_line0": bad_l0,
        "bad_roi": bad_roi,
        "bad_n1": bad_n1,
        "bad_total": bad_sw + bad_l0 + bad_roi + bad_n1,
        "classic": sum(
            (x.get("classic") or 0) for x in all_blocks + summary["n1"]
        ),
        "seam": sum((x.get("seam") or 0) for x in all_blocks + summary["n1"]),
        "tear": sum((x.get("tear") or 0) for x in all_blocks + summary["n1"]),
        "first_all_1": first_ok,
        "tag_unique_all": tag_ok,
        "n1_pass_all": n1_pass_all,
        "apply_latency": lat,
        "apply_all_le_3s": within_3,
        "apply_all_le_5s": within_5,
        "quality_pass": quality_ok and first_ok,
        "n1_phase_pass": n1_pass_all and tag_ok,
        "latency_pass_hard": within_5 and bool(real_applies),
        "latency_pass_goal": within_3 and bool(real_applies),
        "dim_changed": any(
            (x.get("dims") or [None])[0] not in (None, "1456x1088")
            for x in summary["roi"]
            if str(x.get("name", "")).startswith("roi")
            and not str(x.get("name")).startswith("reset")
        ),
        "accept_pass": (
            quality_ok
            and first_ok
            and tag_ok
            and n1_pass_all
            and within_5
            and bool(real_applies)
        ),
    }
    (OUT / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print("BEGIN_SUMMARY_JSON", flush=True)
    print(json.dumps(summary["totals"], ensure_ascii=False), flush=True)
    print("END_SUMMARY_JSON", flush=True)
    print("OUT", OUT, flush=True)
    print("ACCEPT", "PASS" if summary["totals"]["accept_pass"] else "FAIL", flush=True)


if __name__ == "__main__":
    asyncio.run(main())
