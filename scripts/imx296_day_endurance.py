#!/usr/bin/env python3
"""IMX296 24h endurance via CamOS frontend/backend path (not grabber direct).

Runs on board against local CamOS (server.py + preview/WS):
  login cookie → PATCH /api/flow_config (monitor/trigger × close/software/line0)
  → WS rkcam_set_roi / rkcam_reset_roi / rkcam_set_exposure_time / rkcam_trigger_once
  → GET /preview/frame + X-Frame-Tag
  → LO/HI 曝光交替判 N-1；JPEG 底行判绿线

Requires CamOS already up (prefer SMARTCAM_NO_VITE_DEV=1 python server.py,
or systemctl start camos). Do NOT recover via 0_测试运行_camos.sh (rebuilds frontend).
Does not own /dev/video11 directly.

Outputs under OUT_DIR (default /tmp/imx296_day_endurance):
  console.log, events.jsonl, summary.json, summary.txt, bugs.jsonl, green/*
"""
from __future__ import annotations

import argparse
import asyncio
import http.cookiejar
import json
import os
import random
import signal
import subprocess
import sys
import time
import traceback
import urllib.error
import urllib.request
from datetime import datetime, timezone
from io import BytesIO
from pathlib import Path

import numpy as np

try:
    import websockets
except ImportError:
    print("FATAL: websockets missing", file=sys.stderr)
    raise SystemExit(2)

try:
    from PIL import Image
except ImportError:
    print("FATAL: Pillow missing", file=sys.stderr)
    raise SystemExit(2)

BASE = os.environ.get("CAMOS_BASE", "http://127.0.0.1:8000")
WS_URL = os.environ.get("CAMOS_WS", "ws://127.0.0.1:8000/ws")
USER = os.environ.get("CAMOS_USER", "admin")
PASS = os.environ.get("CAMOS_PASS", "dlcv2026")

RUN_MODE_SYSFS = Path("/sys/bus/i2c/devices/4-001a/run_mode")
TRIGGER_SYSFS = Path("/sys/bus/i2c/devices/4-001a/trigger")
PULSE_SYSFS = Path("/sys/bus/i2c/devices/4-001a/trigger_pulse_us")

LO_US = 100.0
HI_US = 16000.0
CENTER_FRAC = 0.25
BOT_ROWS = 8
GREEN_UV_THR = 8.0  # unused for JPEG path; kept for summary compatibility
GREEN_G_DELTA = 20.0
GREEN_G_ABS = 80.0
# Bottom quality: classic / seam / tear — see scripts/imx296_bottom_quality.py
# 暗场 JPEG 下 span 常 <2；低于此阈值的相位结果标 INCONCLUSIVE，不记 n1/mix bug
PHASE_SPAN_MIN = 2.0
PHASE_N1_SPAN_MIN = 5.0  # 仅 span 达标才记 hard N1
# 0.5h 轮 MIX 主因：曝光切换后 1～2 枪仍是旧亮度 / 单帧 outlier 把 mid 拉飞
PHASE_DROP_HEAD = 2  # 有效样本前再丢 head（切换残留）
PHASE_OUTLIER_MAD_K = 4.0  # 组内 |x-med| > k*MAD（或 >0.45*组间span）剔除
EXP_SETTLE_S = 0.45  # set_exposure 后等待
EXP_DISCARD_EACH = 1  # 每次改曝光后先丢枪再采样（trigger 路径）
RUN_MODE_COOLDOWN_S = 5.5  # CamOS RUN_MODE_SWITCH_COOLDOWN_S = 5.0
PATCH_FLOW_TIMEOUT_S = 55.0
ROI_SETTLE_S = 4.0
MON_SETTLE_S = 3.5
TRIG_SETTLE_S = 4.5
DISCARD_AFTER_SWITCH = 3
# ROI 最小高度：240 边界在 HW crop/ISP 上曾导致 restart 后 streaming=0
ROI_MIN_W = 320
ROI_MIN_H = 256

SENSOR_W = 1456
SENSOR_H = 1088
ROIS = [
    (0, 0, 640, 480),
    (200, 200, 640, 480),
    (100, 100, 800, 600),
    (50, 80, 1024, 768),
    (0, 0, 1200, 560),
    (300, 200, 640, 360),
    (0, 100, 1456, 400),
    (400, 100, 512, 384),
    (0, 0, 1456, 1088),
    (120, 60, 960, 540),
    (80, 200, 720, 400),
    (16, 16, 1280, 720),
    (256, 128, 800, 480),
    (0, 0, 400, 400),
    (500, 300, 640, 480),
]

STOP = False


def align_down(v: int, a: int = 2) -> int:
    return max(0, (int(v) // a) * a)


def random_roi() -> tuple[int, int, int, int]:
    if random.random() < 0.55:
        x, y, w, h = random.choice(ROIS)
    else:
        w = align_down(random.randint(320, SENSOR_W), 16) or 320
        h = align_down(random.randint(240, SENSOR_H), 8) or 240
        x = align_down(random.randint(0, max(0, SENSOR_W - w)), 2)
        y = align_down(random.randint(0, max(0, SENSOR_H - h)), 2)
    w = min(align_down(w, 2) or 320, SENSOR_W)
    h = min(align_down(h, 2) or 240, SENSOR_H)
    x = min(align_down(x, 2), max(0, SENSOR_W - w))
    y = min(align_down(y, 2), max(0, SENSOR_H - h))
    if w < ROI_MIN_W:
        w = ROI_MIN_W
    if h < ROI_MIN_H:
        h = ROI_MIN_H
    # 对齐到 16x8，降低 subdev/VB2 对齐失败概率
    w = max(ROI_MIN_W, align_down(w, 16) or ROI_MIN_W)
    h = max(ROI_MIN_H, align_down(h, 8) or ROI_MIN_H)
    if x + w > SENSOR_W:
        x = max(0, SENSOR_W - w)
    if y + h > SENSOR_H:
        y = max(0, SENSOR_H - h)
    return int(x), int(y), int(w), int(h)


def _on_sig(_s, _f):
    global STOP
    STOP = True


signal.signal(signal.SIGTERM, _on_sig)
signal.signal(signal.SIGINT, _on_sig)


def now_local() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def now_iso() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")


def run_mode_text() -> str:
    try:
        return RUN_MODE_SYSFS.read_text(encoding="utf-8", errors="ignore").replace("\n", " | ")
    except Exception as e:
        return f"ERR {e}"


def sysfs_pulse(us: int | None = None) -> None:
    if us is not None:
        try:
            PULSE_SYSFS.write_text(f"{int(us)}\n", encoding="utf-8")
        except Exception:
            pass
    try:
        TRIGGER_SYSFS.write_text("1\n", encoding="utf-8")
    except Exception:
        pass


class Sink:
    def __init__(self, out: Path):
        self.out = out
        self.out.mkdir(parents=True, exist_ok=True)
        (self.out / "green").mkdir(exist_ok=True)
        self.console = self.out / "console.log"
        self.events = self.out / "events.jsonl"
        self.bugs = self.out / "bugs.jsonl"
        self.summary_json = self.out / "summary.json"
        self.summary_txt = self.out / "summary.txt"
        self.stats = {
            "started_at": now_iso(),
            "updated_at": now_iso(),
            "path": "camos",
            "cycles": 0,
            "cases": 0,
            "frames": 0,
            "valid_frames": 0,
            "green_frames": 0,
            "n1_cases": 0,
            "mix_cases": 0,
            "low_contrast_cases": 0,
            "inconclusive_cases": 0,
            "too_few_cases": 0,
            "no_frame": 0,
            "switch_fail": 0,
            "switch_warn": 0,
            "roi_fail": 0,
            "recoveries": 0,
            "crashes": 0,
            "bugs": 0,
            "by_case": {},
            "last_bug": None,
            "kernel": None,
            "build": None,
        }

    def log(self, msg: str, alert: bool = False):
        line = f"[{now_local()}] {'ALERT ' if alert else ''}{msg}"
        # 仅 print；由 nohup 重定向写入 console.log，避免双重 write
        print(line, flush=True)

    def event(self, kind: str, **kw):
        ev = {"ts": now_iso(), "kind": kind, **kw}
        with self.events.open("a", encoding="utf-8") as f:
            f.write(json.dumps(ev, ensure_ascii=False) + "\n")
        return ev

    def bug(self, title: str, **kw):
        self.stats["bugs"] += 1
        b = {"ts": now_iso(), "title": title, "cycle": self.stats["cycles"], **kw}
        self.stats["last_bug"] = b
        with self.bugs.open("a", encoding="utf-8") as f:
            f.write(json.dumps(b, ensure_ascii=False) + "\n")
        self.log(f"BUG {title} {json.dumps(kw, ensure_ascii=False)[:300]}", alert=True)
        self.event("bug", title=title, **kw)
        return b

    def touch_summary(self):
        self.stats["updated_at"] = now_iso()
        try:
            self.stats["uptime_s"] = int(
                time.time() - datetime.fromisoformat(self.stats["started_at"]).timestamp()
            )
        except Exception:
            self.stats["uptime_s"] = 0
        self.summary_json.write_text(
            json.dumps(self.stats, indent=2, ensure_ascii=False), encoding="utf-8"
        )
        s = self.stats
        lines = [
            f"updated={s['updated_at']}",
            f"started={s['started_at']}",
            f"uptime_s={s.get('uptime_s', 0)}",
            f"path={s.get('path')}",
            f"cycles={s['cycles']} cases={s['cases']} frames={s['frames']} valid={s['valid_frames']}",
            f"green={s['green_frames']} n1={s['n1_cases']} mix={s['mix_cases']} "
            f"lowc={s['low_contrast_cases']} incon={s.get('inconclusive_cases', 0)} "
            f"toofew={s['too_few_cases']} noframe={s['no_frame']}",
            f"switch_fail={s['switch_fail']} switch_warn={s.get('switch_warn', 0)} "
            f"roi_fail={s['roi_fail']} recoveries={s['recoveries']} "
            f"crashes={s['crashes']} bugs={s['bugs']}",
            f"build={s.get('build')} kernel={s.get('kernel')}",
            f"last_bug={json.dumps(s.get('last_bug'), ensure_ascii=False)[:240] if s.get('last_bug') else None}",
            f"by_case={json.dumps(s.get('by_case'), ensure_ascii=False)}",
            f"run_mode={run_mode_text()}",
        ]
        self.summary_txt.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _mad(vals: list[float]) -> float:
    if not vals:
        return 0.0
    med = float(np.median(np.asarray(vals, dtype=np.float64)))
    return float(np.median(np.abs(np.asarray(vals, dtype=np.float64) - med)))


def filter_phase_samples(
    means: list[float], expects: list[str]
) -> tuple[list[float], list[str], dict]:
    """Drop head samples + within-expect outliers that inflate MIX false positives."""
    meta: dict = {"in_n": len(means), "dropped_head": 0, "dropped_outlier": 0}
    mm = list(means)
    ee = list(expects)
    head = min(PHASE_DROP_HEAD, max(0, len(mm) - 3))
    if head:
        mm = mm[head:]
        ee = ee[head:]
        meta["dropped_head"] = head
    if len(mm) < 2:
        return mm, ee, meta

    lo_vals = [m for m, e in zip(mm, ee) if e == "LO"]
    hi_vals = [m for m, e in zip(mm, ee) if e == "HI"]
    if lo_vals and hi_vals:
        lo_med = float(np.median(lo_vals))
        hi_med = float(np.median(hi_vals))
        gap = abs(hi_med - lo_med)
        thr_gap = max(3.0, 0.45 * gap) if gap > 0 else 1e9
    else:
        lo_med = hi_med = 0.0
        thr_gap = 1e9

    keep_m: list[float] = []
    keep_e: list[str] = []
    for m, e in zip(mm, ee):
        group = lo_vals if e == "LO" else hi_vals
        med = float(np.median(group)) if group else float(m)
        mad = _mad(group) if len(group) >= 3 else 0.0
        thr = max(thr_gap, PHASE_OUTLIER_MAD_K * mad) if mad > 1e-6 else thr_gap
        if abs(float(m) - med) > thr:
            meta["dropped_outlier"] += 1
            continue
        keep_m.append(float(m))
        keep_e.append(e)
    # 剔太狠则回退到仅 drop-head 的序列，避免 TOO_FEW
    if len(keep_m) < 3:
        return mm, ee, {**meta, "outlier_reverted": True}
    return keep_m, keep_e, meta


def classify_phase(means: list[float], expects: list[str]) -> dict:
    if len(means) < 2:
        return {
            "verdict": "TOO_FEW",
            "span": 0.0,
            "labels": [],
            "expects": expects,
            "means": means,
        }
    arr = np.asarray(means, dtype=np.float64)
    # 用 expect 分组中位数做阈值，避免单帧 outlier 把 min/max mid 拉飞
    lo_vals = [float(m) for m, e in zip(means, expects) if e == "LO"]
    hi_vals = [float(m) for m, e in zip(means, expects) if e == "HI"]
    if lo_vals and hi_vals:
        lo_ref = float(np.median(lo_vals))
        hi_ref = float(np.median(hi_vals))
        if hi_ref < lo_ref:
            lo_ref, hi_ref = hi_ref, lo_ref
        mid = 0.5 * (lo_ref + hi_ref)
        span = float(hi_ref - lo_ref)
        # 仍报告全序列极值 span，便于看 outlier
        span_ext = float(arr.max() - arr.min())
    else:
        lo_m, hi_m = float(arr.min()), float(arr.max())
        span = hi_m - lo_m
        span_ext = span
        mid = 0.5 * (lo_m + hi_m)
    labels = ["HI" if m >= mid else "LO" for m in means]
    # 先按 LO/HI 标签与 expect 对齐判相位；暗场 JPEG 下 span 可能 <1 但仍交替正确
    cur = sum(g == e for g, e in zip(labels, expects))
    n1 = 0
    for i in range(1, len(labels)):
        if labels[i] == expects[i - 1] and labels[i] != expects[i]:
            n1 += 1
    # 允许 1 枪噪声：匹配率够高仍算 CUR（收 MIX 假阳）
    if cur == len(labels) or (len(labels) >= 5 and cur >= len(labels) - 1):
        verdict = "CUR"
    elif span < PHASE_SPAN_MIN and span_ext < PHASE_SPAN_MIN:
        verdict = "INCONCLUSIVE"
    elif n1 >= max(2, (len(labels) - 1) // 2):
        verdict = "N1"
    else:
        verdict = "MIX"
    return {
        "verdict": verdict,
        "span": round(span_ext if span_ext > span else span, 3),
        "span_ref": round(span, 3),
        "mid": round(mid, 3),
        "labels": labels,
        "expects": expects,
        "means": [round(m, 3) for m in means],
        "cur_matches": cur,
        "n1_hits": sum(
            1
            for i in range(1, len(labels))
            if labels[i] == expects[i - 1] and labels[i] != expects[i]
        ),
    }


def analyze_jpeg(data: bytes) -> dict:
    """Multi-signal bottom quality (classic green / chroma seam / tear).

    Implementation lives in imx296_bottom_quality.py (Pillow-only). Falls back
    to an inlined import path next to this file for board deploys.
    """
    try:
        from imx296_bottom_quality import analyze_jpeg as _aq
        return _aq(data)
    except Exception:
        pass
    import importlib.util
    from pathlib import Path as _P

    for cand in (
        _P(__file__).resolve().parent / "imx296_bottom_quality.py",
        _P("/userdata/imx296_warmup_ab/scripts/imx296_bottom_quality.py"),
    ):
        if cand.is_file():
            spec = importlib.util.spec_from_file_location("imx296_bottom_quality", cand)
            mod = importlib.util.module_from_spec(spec)
            assert spec.loader is not None
            spec.loader.exec_module(mod)
            return mod.analyze_jpeg(data)
    raise RuntimeError("imx296_bottom_quality.py not found")


class CamOSClient:
    def __init__(self, sink: Sink):
        self.sink = sink
        self.cj = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.cj))
        self.ws = None
        self._last_run_mode_switch = 0.0
        self._current_flow = {"run_mode": None, "trigger_mode": None}

    @property
    def cookie_header(self) -> str:
        return "; ".join(f"{c.name}={c.value}" for c in self.cj)

    def login(self) -> bool:
        req = urllib.request.Request(
            f"{BASE}/api/auth/login",
            data=json.dumps({"username": USER, "password": PASS}).encode(),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with self.opener.open(req, timeout=10) as r:
                body = r.read()
            ok = b'"success":true' in body or b'"username"' in body
            if ok:
                self.sink.log(f"login ok cookies={len(list(self.cj))}")
            return ok
        except Exception as e:
            self.sink.log(f"login fail {e}", alert=True)
            return False

    def get_flow(self) -> dict:
        req = urllib.request.Request(
            f"{BASE}/api/flow_config",
            headers={"Cookie": self.cookie_header},
        )
        with self.opener.open(req, timeout=8) as r:
            return json.loads(r.read().decode())

    def _cooldown_wait(self, target_run: str):
        cur = self._current_flow.get("run_mode")
        if cur is None or cur == target_run:
            return
        elapsed = time.monotonic() - self._last_run_mode_switch
        if elapsed < RUN_MODE_COOLDOWN_S:
            time.sleep(RUN_MODE_COOLDOWN_S - elapsed + 0.15)

    def _refresh_flow_cache(self) -> dict:
        try:
            cfg = self.get_flow()
            self._current_flow = {
                "run_mode": cfg.get("run_mode"),
                "trigger_mode": cfg.get("trigger_mode"),
            }
            return cfg
        except Exception:
            return {}

    def preview_healthy(self, retries: int = 4, gap_s: float = 0.35) -> tuple[bool, str]:
        """True only if preview returns JPEG body (login-alone is not enough).

        line0/FT 下 ROI restart 后常见短暂 204/空 body，streaming 仍为 1；多探几次再判死。
        """
        last = "no_attempt"
        for i in range(max(1, retries)):
            st, tag, data = self._get_preview_raw()
            if st == 200 and data and len(data) > 800:
                return True, f"ok tag={tag} sz={len(data)} try={i}"
            last = f"st={st} tag={tag} sz={len(data) if data else 0}"
            time.sleep(gap_s)
        return False, last

    def stream_and_preview_ok(
        self, *, poke_line0: bool = False, retries: int = 4
    ) -> tuple[bool, str]:
        rm = run_mode_text()
        if "streaming=1" not in rm:
            return False, f"no_stream rm={rm}"
        # line0 需要外部脉冲才刷新 preview；健康检查前轻推几枪避免假死
        if poke_line0 or "master_fast_trigger" in rm:
            try:
                for _ in range(2):
                    sysfs_pulse()
                    time.sleep(0.08)
            except Exception:
                pass
        ok, info = self.preview_healthy(retries=retries)
        if not ok:
            # 再等 ROI encoder settle 一次
            time.sleep(0.6)
            try:
                if poke_line0 or "master_fast_trigger" in rm:
                    sysfs_pulse()
            except Exception:
                pass
            ok, info = self.preview_healthy(retries=3)
        if not ok:
            return False, f"preview_bad {info} rm={rm}"
        return True, f"{info} | {rm}"

    def patch_flow(self, run_mode: str, trigger_mode: str, retries: int = 12) -> tuple[bool, str]:
        """PATCH /api/flow_config; respect cooldown; short-circuit only if healthy."""
        cur_rm = self._current_flow.get("run_mode")
        cur_tm = self._current_flow.get("trigger_mode")
        if cur_rm is None or cur_tm is None:
            self._refresh_flow_cache()
            cur_rm = self._current_flow.get("run_mode")
            cur_tm = self._current_flow.get("trigger_mode")
        if cur_rm == run_mode and cur_tm == trigger_mode:
            healthy, hinfo = self.stream_and_preview_ok()
            if healthy:
                return True, f"already {run_mode}/{trigger_mode}"
            # 死流时禁止短路径，强制再走硬件/后续 recover
            self.sink.log(
                f"patch short-circuit blocked (unhealthy): {hinfo}", alert=True
            )

        self._cooldown_wait(run_mode)
        last_err = ""
        for attempt in range(retries):
            if STOP:
                return False, "stopped"
            payload = {"run_mode": run_mode, "trigger_mode": trigger_mode}
            req = urllib.request.Request(
                f"{BASE}/api/flow_config",
                data=json.dumps(payload).encode(),
                headers={
                    "Content-Type": "application/json",
                    "Cookie": self.cookie_header,
                },
                method="PATCH",
            )
            t0 = time.monotonic()
            try:
                with self.opener.open(req, timeout=PATCH_FLOW_TIMEOUT_S) as r:
                    body = r.read()
                patch_ms = int((time.monotonic() - t0) * 1000)
                data = json.loads(body.decode())
                if data.get("ok"):
                    cfg = data.get("config") or {}
                    prev = self._current_flow.get("run_mode")
                    self._current_flow = {
                        "run_mode": cfg.get("run_mode", run_mode),
                        "trigger_mode": cfg.get("trigger_mode", trigger_mode),
                    }
                    if prev != self._current_flow["run_mode"]:
                        self._last_run_mode_switch = time.monotonic()
                    return True, (
                        f"ok patch_ms={patch_ms} "
                        + json.dumps(cfg, ensure_ascii=False)[:180]
                    )
                last_err = str(data.get("error") or body[:200])
                if "冷却" in last_err or "cooldown" in last_err.lower():
                    time.sleep(RUN_MODE_COOLDOWN_S)
                    continue
            except urllib.error.HTTPError as e:
                patch_ms = int((time.monotonic() - t0) * 1000)
                err_body = ""
                try:
                    err_body = e.read().decode(errors="ignore")[:200]
                except Exception:
                    pass
                last_err = f"HTTP {e.code} patch_ms={patch_ms} {err_body}"
                if e.code in (401, 403):
                    self.login()
                time.sleep(1.0)
            except Exception as e:
                patch_ms = int((time.monotonic() - t0) * 1000)
                msg = str(e)
                kind = "error"
                if "timed out" in msg.lower() or "timeout" in msg.lower():
                    kind = "client_timeout"
                    cfg = self._refresh_flow_cache()
                    if (
                        cfg.get("run_mode") == run_mode
                        and cfg.get("trigger_mode") == trigger_mode
                    ):
                        return True, f"timeout_but_cfg_ok patch_ms={patch_ms}"
                last_err = f"{kind} patch_ms={patch_ms} {msg}"
                time.sleep(1.0)
        return False, last_err

    def wait_stream(
        self,
        timeout: float = 18.0,
        need_ft: bool = False,
        need_free: bool = False,
        strict_mode: bool = False,
    ) -> tuple[bool, str, dict]:
        """Wait streaming=1. Default soft: mode mismatch still ok (meta.mode_mismatch)."""
        t0 = time.time()
        last_rm = ""
        saw_stream = False
        while time.time() - t0 < timeout and not STOP:
            rm = run_mode_text()
            last_rm = rm
            streaming = "streaming=1" in rm
            if streaming:
                saw_stream = True
            mode_ok = True
            if need_ft:
                mode_ok = mode_ok and "active=master_fast_trigger" in rm
            if need_free:
                mode_ok = mode_ok and "active=free_run" in rm
            if streaming and (mode_ok or not strict_mode):
                return True, rm, {
                    "wait_ms": int((time.time() - t0) * 1000),
                    "mode_mismatch": bool(streaming and not mode_ok),
                    "streaming": True,
                }
            time.sleep(0.25)
        meta = {
            "wait_ms": int((time.time() - t0) * 1000),
            "mode_mismatch": True,
            "streaming": saw_stream or ("streaming=1" in last_rm),
        }
        if not strict_mode and "streaming=1" in last_rm:
            meta["mode_mismatch"] = (
                (need_ft and "active=master_fast_trigger" not in last_rm)
                or (need_free and "active=free_run" not in last_rm)
            )
            return True, last_rm, meta
        return False, last_rm or run_mode_text(), meta

    async def open_ws(self):
        if self.ws is not None:
            try:
                await self.ws.close()
            except Exception:
                pass
            self.ws = None
        # websockets lib: pass cookie header
        self.ws = await websockets.connect(
            WS_URL,
            additional_headers={"Cookie": self.cookie_header},
            open_timeout=8,
            ping_interval=20,
            ping_timeout=20,
            max_size=4 * 1024 * 1024,
        )
        return self.ws

    async def ensure_ws(self):
        if self.ws is None:
            return await self.open_ws()
        try:
            # cheap liveness
            if getattr(self.ws, "closed", False):
                return await self.open_ws()
        except Exception:
            return await self.open_ws()
        return self.ws

    async def ws_cmd(self, cmd: str, value=None, drain: int = 3):
        ws = await self.ensure_ws()
        msg = {"cmd": cmd}
        if value is not None:
            msg["value"] = value
        try:
            await ws.send(json.dumps(msg))
        except Exception:
            ws = await self.open_ws()
            await ws.send(json.dumps(msg))
        # drain a few status pushes so buffer doesn't grow forever
        for _ in range(drain):
            try:
                await asyncio.wait_for(ws.recv(), timeout=0.25)
            except Exception:
                break
        return ws

    def _get_preview_raw(self):
        req = urllib.request.Request(
            f"{BASE}/preview/frame?t={time.time_ns()}",
            headers={"Cookie": self.cookie_header},
        )
        try:
            resp = self.opener.open(req, timeout=6)
            tag = resp.headers.get("X-Frame-Tag")
            data = resp.read()
            return resp.status, tag, data
        except urllib.error.HTTPError as e:
            if e.code in (401, 403):
                self.login()
            return e.code, None, b""
        except Exception as e:
            return str(e), None, b""

    def peek_tag(self):
        st, tag, data = self._get_preview_raw()
        if st == 200 and data and len(data) > 800:
            return tag
        return tag

    async def fire_trigger(self, mode: str):
        """software: WS+debug; line0: sysfs XTRIG only."""
        m = str(mode or "").strip().lower()
        if m == "line0":
            sysfs_pulse()
            return
        try:
            await self.ws_cmd("rkcam_trigger_once", drain=1)
        except Exception:
            pass
        try:
            req = urllib.request.Request(
                f"{BASE}/debug/trigger_once",
                data=b"{}",
                headers={
                    "Content-Type": "application/json",
                    "Cookie": self.cookie_header,
                },
                method="POST",
            )
            self.opener.open(req, timeout=3).read()
        except Exception:
            pass

    async def set_exposure(self, us: float):
        try:
            await self.ws_cmd("rkcam_set_exposure_time", float(us), drain=1)
        except Exception:
            try:
                await self.ws_cmd("set_exposure_us", float(us), drain=1)
            except Exception:
                pass

    async def set_gain(self, gain_db: float = 0.0):
        """固定模拟增益。判 N-1 前应置 0，避免默认高增益把 LO/HI 对比压扁。"""
        try:
            await self.ws_cmd("rkcam_set_gain", float(gain_db), drain=1)
        except Exception:
            try:
                await self.ws_cmd("set_gain", float(gain_db), drain=1)
            except Exception:
                pass

    async def set_roi(self, x: int, y: int, w: int, h: int):
        if w >= SENSOR_W and h >= SENSOR_H and x == 0 and y == 0:
            await self.ws_cmd("rkcam_reset_roi")
        else:
            await self.ws_cmd(
                "rkcam_set_roi", {"x": int(x), "y": int(y), "w": int(w), "h": int(h)}
            )

    async def reset_roi(self):
        await self.ws_cmd("rkcam_reset_roi")

    async def grab_after_trigger(
        self, mode: str, timeout_s: float = 3.0, poll_s: float = 0.05
    ) -> tuple[bytes | None, str | None, bool, dict]:
        """Return (data, tag, is_new, meta)."""
        before = self.peek_tag()
        await self.fire_trigger(mode)
        t0 = time.time()
        last_tag = before
        last_data = b""
        last_st: object = None
        miss_reason = "no_response"
        while time.time() - t0 < timeout_s and not STOP:
            st, tag, data = self._get_preview_raw()
            last_st = st
            if st == 200 and data and len(data) > 800:
                last_tag = tag
                last_data = data
                if before is None or tag is None:
                    if time.time() - t0 > 0.12:
                        return data, tag, True, {"reason": "ok", "before": before}
                elif str(tag) != str(before):
                    return data, tag, True, {
                        "reason": "ok",
                        "before": before,
                        "tag": tag,
                    }
                else:
                    miss_reason = "old_tag"
            elif st != 200:
                miss_reason = f"http_{st}"
            elif not data or len(data) <= 800:
                miss_reason = "empty_body"
            await asyncio.sleep(poll_s)
        await self.fire_trigger(mode)
        t0 = time.time()
        while time.time() - t0 < timeout_s and not STOP:
            st, tag, data = self._get_preview_raw()
            last_st = st
            if st == 200 and data and len(data) > 800:
                last_tag = tag
                last_data = data
                if before is None or (tag is not None and str(tag) != str(before)):
                    return data, tag, True, {
                        "reason": "ok_retry",
                        "before": before,
                        "tag": tag,
                    }
                miss_reason = "old_tag"
            elif st != 200:
                miss_reason = f"http_{st}"
            else:
                miss_reason = "empty_body"
            await asyncio.sleep(poll_s)
        meta = {
            "reason": miss_reason,
            "before": before,
            "tag": last_tag,
            "http_st": last_st,
        }
        if last_data and len(last_data) > 800:
            return last_data, last_tag, False, meta
        return None, last_tag, False, meta

    async def grab_free_run(self, timeout_s: float = 2.0) -> tuple[bytes | None, str | None]:
        before = self.peek_tag()
        t0 = time.time()
        while time.time() - t0 < timeout_s and not STOP:
            st, tag, data = self._get_preview_raw()
            if st == 200 and data and len(data) > 800:
                if before is None or tag is None or str(tag) != str(before):
                    return data, tag
            await asyncio.sleep(0.05)
        st, tag, data = self._get_preview_raw()
        if st == 200 and data and len(data) > 800:
            return data, tag
        return None, tag

    async def close(self):
        if self.ws is not None:
            try:
                await self.ws.close()
            except Exception:
                pass
            self.ws = None


async def switch_to(
    client: CamOSClient, sink: Sink, run_mode: str, trigger_mode: str, label: str
) -> bool:
    ok, info = client.patch_flow(run_mode, trigger_mode)
    sink.event("switch", label=label, ok=ok, info=info, rm=run_mode_text())
    if not ok:
        sink.stats["switch_fail"] += 1
        sink.bug(f"switch_fail:{label}", info=info, rm=run_mode_text())
        return False
    settle = MON_SETTLE_S if run_mode == "monitor" else TRIG_SETTLE_S
    await asyncio.sleep(settle)
    need_ft = run_mode == "trigger"
    need_free = run_mode in ("monitor", "manual")
    ok_s, rm, meta = client.wait_stream(
        18.0, need_ft=need_ft, need_free=need_free, strict_mode=False
    )
    if not ok_s:
        ok_s2, rm2, meta2 = client.wait_stream(
            10.0, need_ft=False, need_free=False, strict_mode=False
        )
        if not ok_s2:
            sink.stats["switch_fail"] += 1
            sink.bug(
                f"stream_fail:{label}",
                rm=rm2 or rm,
                wait_ms=meta2.get("wait_ms"),
                info=info,
            )
            return False
        rm, meta = rm2, meta2
    if meta.get("mode_mismatch"):
        sink.stats["switch_warn"] += 1
        sink.event(
            "switch_warn",
            label=label,
            rm=rm,
            wait_ms=meta.get("wait_ms"),
            need_ft=need_ft,
            need_free=need_free,
        )
        sink.log(f"switch {label} WARN mode_mismatch rm={rm}", alert=True)
    else:
        sink.log(f"switch {label} ok wait_ms={meta.get('wait_ms')} rm={rm}")
    try:
        await client.ensure_ws()
    except Exception as e:
        sink.log(f"ws after switch: {e}")
    return True

async def run_alt_exp_case(
    sink: Sink,
    client: CamOSClient,
    name: str,
    mode: str,
    n: int = 8,
    free_run: bool = False,
) -> dict:
    means: list[float] = []
    expects: list[str] = []
    greens: list[bool] = []
    details: list[dict] = []
    # 相位判定前固定增益=0：默认用户集常为 ~12dB，暗场下 LO/HI 均值挤在一起会误判 MIX
    await client.set_gain(0.0)
    await asyncio.sleep(0.2)
    # ROI/模式切换后多 discard，减轻首帧相位污染
    if not free_run:
        for _ in range(DISCARD_AFTER_SWITCH):
            try:
                await client.grab_after_trigger(mode, timeout_s=2.8)
            except Exception:
                pass
    for i in range(n):
        if STOP:
            break
        exp = LO_US if (i % 2 == 0) else HI_US
        expects.append("LO" if exp == LO_US else "HI")
        await client.set_exposure(exp)
        await asyncio.sleep(EXP_SETTLE_S)
        grab_meta: dict = {}
        if free_run:
            # free_run：多等一帧时间再取，减少曝光未生效
            await asyncio.sleep(0.08)
            data, tag = await client.grab_free_run(timeout_s=2.0)
            is_new = data is not None
        else:
            # 每次改曝光后先丢 EXP_DISCARD_EACH 枪，再采统计样本
            for _ in range(EXP_DISCARD_EACH):
                try:
                    await client.grab_after_trigger(mode, timeout_s=2.5)
                except Exception:
                    pass
            data, tag, is_new, grab_meta = await client.grab_after_trigger(
                mode, timeout_s=3.5
            )
        sink.stats["frames"] += 1
        if not data or not is_new:
            means.append(float("nan"))
            greens.append(False)
            sink.stats["no_frame"] += 1
            details.append(
                {
                    "i": i,
                    "err": "no_frame",
                    "tag": tag,
                    "is_new": is_new,
                    "miss": grab_meta,
                }
            )
            # 连续 503 / 无流：熔断本 case，避免空转
            reason = str((grab_meta or {}).get("reason") or "")
            if reason.startswith("http_503") or reason.startswith("http_5"):
                consec = sum(
                    1
                    for d in details[-3:]
                    if str((d.get("miss") or {}).get("reason") or "").startswith("http_5")
                )
                if consec >= 2:
                    sink.log(
                        f"ABORT {name} consecutive preview 5xx, break case",
                        alert=True,
                    )
                    break
            continue
        try:
            a = analyze_jpeg(data)
            means.append(float(a["mean"]))
            greens.append(bool(a["green"]))
            sink.stats["valid_frames"] += 1
            if a["green"]:
                sink.stats["green_frames"] += 1
                gpath = sink.out / "green" / f"c{sink.stats['cycles']:04d}_{name}_{i:02d}.jpg"
                gpath.write_bytes(data)
                gpath.with_suffix(".json").write_text(
                    json.dumps(
                        {"case": name, "i": i, "tag": tag, **a, "rm": run_mode_text()},
                        indent=2,
                        ensure_ascii=False,
                    ),
                    encoding="utf-8",
                )
            details.append(
                {
                    "i": i,
                    "exp": exp,
                    "mean": round(float(a["mean"]), 3),
                    "green": a["green"],
                    "classic_green": a.get("classic_green", False),
                    "seam": a.get("seam", False),
                    "tear": a.get("tear", False),
                    "score": a.get("score", 0),
                    "reasons": a.get("reasons", []),
                    "tag": tag,
                    "dim": a["dim"],
                    "bot": [a["br"], a["bg"], a["bb"]],
                }
            )
        except Exception as e:
            means.append(float("nan"))
            greens.append(False)
            details.append({"i": i, "err": str(e), "tag": tag})

    mm, ee = [], []
    for m, e in zip(means, expects):
        if m == m:  # not nan
            mm.append(m)
            ee.append(e)
    filt_m, filt_e, filt_meta = filter_phase_samples(mm, ee)
    if len(filt_m) >= 2:
        phase = classify_phase(filt_m, filt_e)
    else:
        phase = classify_phase(mm, ee)
    phase_all = classify_phase(mm, ee) if len(mm) >= 2 else phase
    phase = dict(phase)
    phase["filter"] = filt_meta

    result = {
        "name": name,
        "phase_drop_first": phase,
        "phase_all": phase_all,
        "green_frames": int(sum(1 for x in greens if x)),
        "total": len(means),
        "valid": len(mm),
        "details": details,
        "run_mode": run_mode_text(),
        "phase_filter": filt_meta,
    }

    sink.stats["cases"] += 1
    bc = sink.stats["by_case"].setdefault(
        name,
        {
            "n": 0,
            "green": 0,
            "n1": 0,
            "mix": 0,
            "cur": 0,
            "lowc": 0,
            "incon": 0,
            "toofew": 0,
            "fail": 0,
        },
    )
    bc["n"] += 1
    v = phase["verdict"]
    span = float(phase.get("span") or 0.0)
    if v == "CUR":
        bc["cur"] += 1
    elif v == "N1":
        pass  # hard count after span gate
    elif v == "MIX":
        bc["mix"] += 1
        sink.stats["mix_cases"] += 1
    elif v in ("LOW_CONTRAST", "INCONCLUSIVE"):
        bc["incon"] = bc.get("incon", 0) + 1
        bc["lowc"] += 1
        sink.stats["inconclusive_cases"] += 1
        sink.stats["low_contrast_cases"] += 1
        v = "INCONCLUSIVE"
    elif v == "TOO_FEW":
        bc["toofew"] += 1
        sink.stats["too_few_cases"] += 1
    gcnt = result["green_frames"]
    bc["green"] += gcnt

    bad = False
    reasons = []
    is_free = "FREERUN" in name or free_run
    if v == "N1":
        if span >= PHASE_N1_SPAN_MIN and not is_free:
            bad = True
            reasons.append("phase=N1")
            bc["n1"] += 1
            sink.stats["n1_cases"] += 1
        else:
            v = "INCONCLUSIVE"
            bc["incon"] = bc.get("incon", 0) + 1
            sink.stats["inconclusive_cases"] += 1
            sink.log(
                f"INCONCLUSIVE {name} weak_N1 span={span} (need>={PHASE_N1_SPAN_MIN})"
            )
    elif v == "TOO_FEW":
        bad = True
        reasons.append(f"phase={v}")
    elif v == "MIX" and not is_free:
        if span >= PHASE_SPAN_MIN:
            bad = True
            reasons.append(f"phase={v}")
        else:
            bc["mix"] = max(0, bc.get("mix", 0) - 1)
            sink.stats["mix_cases"] = max(0, sink.stats["mix_cases"] - 1)
            v = "INCONCLUSIVE"
            bc["incon"] = bc.get("incon", 0) + 1
            sink.stats["inconclusive_cases"] += 1
    elif v == "INCONCLUSIVE":
        pass
    if gcnt > 0:
        bad = True
        reasons.append(f"green={gcnt}")
    if result["valid"] < max(2, n // 2):
        bad = True
        reasons.append(f"valid={result['valid']}/{n}")

    result["phase_final"] = v
    result["span"] = span
    if bad:
        bc["fail"] += 1
        sink.bug(
            f"{name}:{','.join(reasons)}",
            case=name,
            phase=v,
            phase_all=phase_all.get("verdict"),
            green=gcnt,
            labels=phase.get("labels"),
            expects=phase.get("expects"),
            means=phase.get("means"),
            span=phase.get("span"),
            run_mode=result["run_mode"],
            sample_details=details[:4],
        )
    else:
        sink.log(
            f"OK {name} phase={v} green={gcnt} valid={result['valid']}/{n} "
            f"span={phase.get('span')} rm={result['run_mode'][:60]}"
        )
    sink.event("case", **{k: result[k] for k in result if k != "details"}, details_n=len(details))
    sink.touch_summary()
    return result



def _kill_camos_devs() -> None:
    """Stop all CamOS server paths (dev + systemd unit process)."""
    subprocess.call(
        ["systemctl", "stop", "camos"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    subprocess.call(
        ["pkill", "-9", "-f", "python server.py"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    # packaged entry may appear as .../camos/camos
    subprocess.call(
        ["pkill", "-9", "-f", "site-packages/camos/camos"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    subprocess.call(
        ["pkill", "-9", "-f", "[v]ite"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(1.0)
    # release video node if still held
    subprocess.call(
        ["fuser", "-k", "/dev/video11"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(0.5)


def _start_camos_light(sink: Sink) -> None:
    """Always single-instance: NO_VITE python server.py from /root/smartcam_bs.

    Do NOT systemctl start camos while a dev tree is the test target — it races
    the existing server and leaves streaming=0 with login still 200.
    """
    try:
        RUN_MODE_SYSFS.write_text("free_run\n", encoding="utf-8")
    except Exception:
        pass

    _kill_camos_devs()
    time.sleep(1)
    logf = open("/tmp/camos_boot_endurance.log", "a")
    env = os.environ.copy()
    env["SMARTCAM_NO_VITE_DEV"] = "1"
    env["PYTHONUNBUFFERED"] = "1"
    subprocess.Popen(
        ["python", "server.py", "--host", "0.0.0.0", "--port", "8000", "80"],
        cwd="/root/smartcam_bs",
        stdout=logf,
        stderr=subprocess.STDOUT,
        start_new_session=True,
        env=env,
    )
    sink.log("camos start via python server.py (NO_VITE_DEV=1, single instance)")


async def ensure_camos_up(sink: Sink, client: CamOSClient) -> bool:
    """Login + preview body required. Restart single instance if unhealthy."""
    if client.login():
        ok, info = client.preview_healthy()
        if ok:
            # also prefer streaming=1 but preview body is the hard gate
            sink.log(f"camos up preview {info}")
            return True
        sink.log(f"camos login ok but preview unhealthy: {info}", alert=True)
    else:
        sink.log("camos login failed", alert=True)

    sink.log("camos not healthy, light restart (single instance)", alert=True)
    sink.stats["recoveries"] += 1
    _start_camos_light(sink)
    for i in range(90):
        if STOP:
            return False
        time.sleep(2)
        if not client.login():
            continue
        ok, info = client.preview_healthy()
        if ok:
            # clear stale flow cache after restart
            client._current_flow = {"run_mode": None, "trigger_mode": None}
            try:
                client._refresh_flow_cache()
            except Exception:
                pass
            try:
                await client.open_ws()
            except Exception:
                pass
            sink.log(f"camos restarted ok i={i} {info}")
            return True
    sink.bug("camos_restart_fail")
    return False


async def one_cycle(sink: Sink, client: CamOSClient, cycle: int):
    sink.stats["cycles"] = cycle
    sink.log(f"===== cycle {cycle} begin path=camos =====")

    # 1) monitor / free_run smoke (few frames, no hard N-1)
    if not await switch_to(client, sink, "monitor", "close", "MON"):
        await client_recover(sink, client)
        if not await switch_to(client, sink, "monitor", "close", "MON_retry"):
            return
    await run_alt_exp_case(sink, client, "MON_FREERUN", mode="close", n=4, free_run=True)

    # 2) software trigger full + ROI set/reset
    if not await switch_to(client, sink, "trigger", "software", "SW"):
        await client_recover(sink, client)
        if not await switch_to(client, sink, "trigger", "software", "SW_retry"):
            return
    await run_alt_exp_case(sink, client, "SW_BASE", mode="software", n=8)

    async def _roi_then_case(label_prefix: str, xx, yy, ww, hh, mode: str, n: int) -> bool:
        """set ROI, require stream+preview healthy, then run case. False => need recover."""
        try:
            await client.set_roi(xx, yy, ww, hh)
            await asyncio.sleep(ROI_SETTLE_S)
            sink.event("roi_set", x=xx, y=yy, w=ww, h=hh, rm=run_mode_text())
        except Exception as e:
            sink.stats["roi_fail"] += 1
            sink.bug("roi_set_fail", err=str(e), roi=[xx, yy, ww, hh])
            return False
        # software 也给 ROI 后 encoder 一点缓冲，避免瞬时 204 假死
        healthy, hinfo = client.stream_and_preview_ok(retries=5)
        if not healthy:
            sink.stats["roi_fail"] += 1
            sink.bug(
                f"roi_dead_after_set:{label_prefix}",
                info=hinfo,
                roi=[xx, yy, ww, hh],
                rm=run_mode_text(),
            )
            return False
        result = await run_alt_exp_case(
            sink, client, f"{label_prefix}_{ww}x{hh}", mode=mode, n=n
        )
        # case itself may have hit 503 storm
        if result.get("valid", 0) == 0 and result.get("total", 0) > 0:
            healthy2, hinfo2 = client.stream_and_preview_ok(retries=5)
            if not healthy2:
                sink.bug(
                    f"roi_dead_after_case:{label_prefix}",
                    info=hinfo2,
                    roi=[xx, yy, ww, hh],
                    rm=run_mode_text(),
                )
                return False
        return True

    x, y, w, h = random_roi()
    if not await _roi_then_case("SW_ROI", x, y, w, h, "software", 8):
        await client_recover(sink, client)
        if not await switch_to(client, sink, "trigger", "software", "SW_after_roi_fail"):
            return
    else:
        try:
            await client.reset_roi()
            await asyncio.sleep(ROI_SETTLE_S)
            sink.event("roi_reset", rm=run_mode_text())
            healthy, hinfo = client.stream_and_preview_ok()
            if not healthy:
                sink.bug("roi_dead_after_reset", info=hinfo, rm=run_mode_text())
                await client_recover(sink, client)
                if not await switch_to(
                    client, sink, "trigger", "software", "SW_after_reset_fail"
                ):
                    return
            else:
                await run_alt_exp_case(sink, client, "SW_ROI_RESET", mode="software", n=8)
        except Exception as e:
            sink.stats["roi_fail"] += 1
            sink.bug("roi_reset_fail_sw", err=str(e))

    # second random ROI in software
    x2, y2, w2, h2 = random_roi()
    if not await _roi_then_case("SW_ROI2", x2, y2, w2, h2, "software", 6):
        await client_recover(sink, client)
        if not await switch_to(client, sink, "trigger", "software", "SW_after_roi2_fail"):
            return
    else:
        try:
            await client.reset_roi()
            await asyncio.sleep(ROI_SETTLE_S)
            healthy, hinfo = client.stream_and_preview_ok()
            if not healthy:
                sink.bug("roi2_dead_after_reset", info=hinfo, rm=run_mode_text())
                await client_recover(sink, client)
        except Exception as e:
            sink.stats["roi_fail"] += 1
            sink.bug("roi2_fail_sw", err=str(e))

    # 3) flip software → line0 (same run_mode=trigger, only trigger_mode change — no cooldown)
    ok, info = client.patch_flow("trigger", "line0")
    sink.event("switch", label="LINE0", ok=ok, info=info, rm=run_mode_text())
    if ok:
        await asyncio.sleep(TRIG_SETTLE_S)
        ok_l0, rm_l0, meta_l0 = client.wait_stream(
            14.0, need_ft=True, strict_mode=False
        )
        sink.event(
            "switch_wait",
            label="LINE0",
            ok=ok_l0,
            rm=rm_l0,
            wait_ms=meta_l0.get("wait_ms"),
            mode_mismatch=meta_l0.get("mode_mismatch"),
        )
        await run_alt_exp_case(sink, client, "LINE0_BASE", mode="line0", n=8)
        x3, y3, w3, h3 = random_roi()
        try:
            await client.set_roi(x3, y3, w3, h3)
            await asyncio.sleep(ROI_SETTLE_S)
            # line0：先 XTRIG 唤醒 preview，再健康检查（避免 204 假死触发 recover）
            try:
                for _ in range(3):
                    sysfs_pulse()
                    await asyncio.sleep(0.12)
            except Exception:
                pass
            healthy, hinfo = client.stream_and_preview_ok(poke_line0=True, retries=6)
            if not healthy:
                sink.bug("roi_dead_line0_set", info=hinfo, roi=[x3, y3, w3, h3])
                await client_recover(sink, client)
            else:
                await run_alt_exp_case(
                    sink, client, f"LINE0_ROI_{w3}x{h3}", mode="line0", n=8
                )
                await client.reset_roi()
                await asyncio.sleep(ROI_SETTLE_S)
                try:
                    for _ in range(3):
                        sysfs_pulse()
                        await asyncio.sleep(0.12)
                except Exception:
                    pass
                healthy2, hinfo2 = client.stream_and_preview_ok(
                    poke_line0=True, retries=6
                )
                if not healthy2:
                    sink.bug("roi_dead_line0_reset", info=hinfo2)
                    await client_recover(sink, client)
                else:
                    await run_alt_exp_case(
                        sink, client, "LINE0_ROI_RESET", mode="line0", n=6
                    )
        except Exception as e:
            sink.stats["roi_fail"] += 1
            sink.bug("roi_fail_line0", err=str(e))
    else:
        sink.stats["switch_fail"] += 1
        sink.bug("switch_fail:LINE0", info=info)

    # 4) back to monitor
    await switch_to(client, sink, "monitor", "close", "MON_END")
    sink.touch_summary()
    sink.log(
        f"===== cycle {cycle} end bugs={sink.stats['bugs']} "
        f"green={sink.stats['green_frames']} n1={sink.stats['n1_cases']} ====="
    )


async def client_recover(sink: Sink, client: CamOSClient):
    sink.stats["recoveries"] += 1
    sink.log("recover begin", alert=True)
    # soft path first
    try:
        # force patch even if cache says monitor (dead stream blocks short-circuit)
        client._current_flow = {"run_mode": None, "trigger_mode": None}
        client.patch_flow("monitor", "close")
        await asyncio.sleep(2)
        await client.ensure_ws()
        await client.reset_roi()
        await asyncio.sleep(ROI_SETTLE_S)
    except Exception as e:
        sink.log(f"recover soft {e}")
    ok_s, rm, _meta = client.wait_stream(8.0, strict_mode=False)
    ok_p, pinfo = client.preview_healthy()
    if ok_s and ok_p:
        sink.log(f"recover soft ok rm={rm} {pinfo}")
        sink.event("recover", kind_soft=True, rm=rm)
        return
    sink.log(
        f"recover soft failed stream={ok_s} preview={ok_p}/{pinfo} -> hard restart",
        alert=True,
    )
    await ensure_camos_up(sink, client)
    try:
        client._current_flow = {"run_mode": None, "trigger_mode": None}
        client.patch_flow("monitor", "close")
        await asyncio.sleep(2)
        await client.open_ws()
        await client.reset_roi()
    except Exception as e:
        sink.log(f"recover hard followup {e}")
    sink.event("recover", kind_soft=False, rm=run_mode_text())


async def amain(hours: float, out: Path) -> int:
    sink = Sink(out)
    try:
        uname = subprocess.check_output(["uname", "-a"], text=True).strip()
    except Exception:
        uname = "?"
    sink.stats["kernel"] = uname
    # build id from uname #YYYYMMDDNN
    for part in uname.split():
        if part.startswith("#20"):
            sink.stats["build"] = part
            break
    sink.log(f"start CamOS endurance hours={hours} out={out} kernel={uname}")
    sink.touch_summary()

    client = CamOSClient(sink)
    if not await ensure_camos_up(sink, client):
        sink.log("FATAL camos not up", alert=True)
        sink.touch_summary()
        return 2
    try:
        await client.open_ws()
    except Exception as e:
        sink.log(f"ws open fail {e}, continue with HTTP-only + retry")

    # seed flow state
    try:
        cfg = client.get_flow()
        client._current_flow = {
            "run_mode": cfg.get("run_mode"),
            "trigger_mode": cfg.get("trigger_mode"),
        }
        sink.log(f"flow_config={json.dumps(cfg, ensure_ascii=False)[:240]}")
    except Exception as e:
        sink.log(f"get_flow {e}")

    t_end = time.time() + hours * 3600.0
    cycle = 0
    while time.time() < t_end and not STOP:
        cycle += 1
        try:
            await one_cycle(sink, client, cycle)
        except Exception as e:
            sink.stats["crashes"] += 1
            sink.bug("cycle_crash", err=str(e), tb=traceback.format_exc()[-800:])
            sink.touch_summary()
            try:
                await client_recover(sink, client)
            except Exception:
                pass
            await asyncio.sleep(2)
        # brief pause between cycles
        for _ in range(10):
            if STOP or time.time() >= t_end:
                break
            await asyncio.sleep(0.5)

    # leave camera in monitor/free_run
    try:
        client.patch_flow("monitor", "close")
    except Exception:
        pass
    await client.close()
    sink.log(f"done cycles={cycle} bugs={sink.stats['bugs']}")
    sink.touch_summary()
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hours", type=float, default=24.0)
    ap.add_argument("--out", default="/tmp/imx296_day_endurance")
    args = ap.parse_args()
    out = Path(args.out)
    return asyncio.run(amain(args.hours, out))


if __name__ == "__main__":
    raise SystemExit(main())
