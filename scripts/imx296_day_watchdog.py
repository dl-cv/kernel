#!/usr/bin/env python3
"""Host-side watchdog for board IMX296 day endurance.

SSH to root@192.168.1.180, pull summary/bugs, append local markdown report,
restart board test if dead (optional).
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import time
from datetime import datetime
from pathlib import Path

HOST = "root@192.168.1.180"
BOARD_OUT = "/tmp/imx296_day_endurance"
BOARD_SCRIPT = "/tmp/imx296_day_endurance.py"
SSH_BASE = [
    "ssh",
    "-o",
    "StrictHostKeyChecking=no",
    "-o",
    "ConnectTimeout=8",
    "-o",
    "BatchMode=yes",
    "-o",
    "ServerAliveInterval=5",
    "-o",
    "ServerAliveCountMax=2",
    HOST,
]


def ts() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def ssh(script: str, timeout: int = 60) -> str:
    try:
        r = subprocess.run(
            SSH_BASE + ["bash", "-s"],
            input=script,
            text=True,
            capture_output=True,
            timeout=timeout,
        )
        out = r.stdout or ""
        if r.stderr:
            out += "\n" + r.stderr
        return out
    except subprocess.TimeoutExpired:
        return "SSH_TIMEOUT"
    except Exception as e:
        return f"SSH_ERR:{e}"


def scp_pull(remote: str, local: Path, timeout: int = 45) -> bool:
    local.parent.mkdir(parents=True, exist_ok=True)
    try:
        r = subprocess.run(
            [
                "scp",
                "-o",
                "StrictHostKeyChecking=no",
                "-o",
                "ConnectTimeout=8",
                "-o",
                "BatchMode=yes",
                f"{HOST}:{remote}",
                str(local),
            ],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        return r.returncode == 0 and local.is_file()
    except Exception:
        return False


def _parse_summary_txt(txt: str) -> dict:
    """Parse key=value lines from summary.txt into a compact dict."""
    s: dict = {}
    for ln in (txt or "").splitlines():
        if "=" not in ln:
            continue
        k, v = ln.split("=", 1)
        k, v = k.strip(), v.strip()
        if k in (
            "cycles",
            "bugs",
            "green_frames",
            "n1_cases",
            "mix_cases",
            "low_contrast_cases",
            "too_few_cases",
            "no_frame",
            "switch_fail",
            "roi_fail",
            "recoveries",
            "crashes",
            "cases",
            "frames",
            "valid_frames",
            "uptime_s",
        ):
            # lines may pack multiple keys; handle single-key first
            try:
                s[k] = int(re.sub(r"[^\d-]", "", v.split()[0]) or 0)
            except Exception:
                pass
        elif k in ("started_at", "updated_at", "build", "kernel", "last_bug", "run_mode", "by_case"):
            s[k] = v
        elif k == "green":
            # green=0 n1=0 mix=0 ... packed line from summary.txt
            for part in ln.replace(",", " ").split():
                if "=" in part:
                    pk, pv = part.split("=", 1)
                    try:
                        if pk == "green":
                            s["green_frames"] = int(pv)
                        elif pk == "n1":
                            s["n1_cases"] = int(pv)
                        elif pk == "mix":
                            s["mix_cases"] = int(pv)
                        elif pk == "lowc":
                            s["low_contrast_cases"] = int(pv)
                        elif pk == "toofew":
                            s["too_few_cases"] = int(pv)
                        elif pk == "noframe":
                            s["no_frame"] = int(pv)
                        elif pk in (
                            "cycles",
                            "cases",
                            "frames",
                            "bugs",
                            "switch_fail",
                            "roi_fail",
                            "recoveries",
                            "crashes",
                        ):
                            s[pk if pk != "frames" else "frames"] = int(pv)
                        elif pk == "valid":
                            s["valid_frames"] = int(pv)
                    except Exception:
                        pass
        elif ln.startswith("cycles=") or " cycles=" in f" {ln}":
            for part in ln.replace(",", " ").split():
                if "=" in part:
                    pk, pv = part.split("=", 1)
                    try:
                        if pk == "cycles":
                            s["cycles"] = int(pv)
                        elif pk == "cases":
                            s["cases"] = int(pv)
                        elif pk == "frames":
                            s["frames"] = int(pv)
                        elif pk == "valid":
                            s["valid_frames"] = int(pv)
                    except Exception:
                        pass
        elif ln.startswith("switch_fail=") or ln.startswith("green="):
            for part in ln.replace(",", " ").split():
                if "=" in part:
                    pk, pv = part.split("=", 1)
                    try:
                        mapping = {
                            "switch_fail": "switch_fail",
                            "roi_fail": "roi_fail",
                            "recoveries": "recoveries",
                            "crashes": "crashes",
                            "bugs": "bugs",
                            "green": "green_frames",
                            "n1": "n1_cases",
                            "mix": "mix_cases",
                            "lowc": "low_contrast_cases",
                            "toofew": "too_few_cases",
                            "noframe": "no_frame",
                        }
                        if pk in mapping:
                            s[mapping[pk]] = int(pv)
                    except Exception:
                        pass
    return s


def collect() -> dict:
    out = ssh(
        f"""
set +e
pid=$(pgrep -f '{BOARD_SCRIPT}' | head -1)
echo PID=$pid
if [ -f {BOARD_OUT}/summary.txt ]; then
  echo '---SUMMARY---'
  cat {BOARD_OUT}/summary.txt
  echo '---END_SUMMARY---'
fi
bugs_n=$(wc -l < {BOARD_OUT}/bugs.jsonl 2>/dev/null || echo 0)
echo BUGS_N=$bugs_n
if [ -f {BOARD_OUT}/bugs.jsonl ]; then
  echo '---BUGS_TAIL---'
  tail -n 8 {BOARD_OUT}/bugs.jsonl
  echo '---END_BUGS---'
fi
echo '---RMODE---'
cat /sys/bus/i2c/devices/4-001a/run_mode 2>/dev/null | tr '\\n' ' '
echo
echo '---UNAME---'
uname -a
echo '---DMESG---'
dmesg 2>/dev/null | grep -E 'SError|Oops|BUG:|n1trace early ceiling-force|ignored XTRIG' | tail -n 8
echo '---END---'
""",
        timeout=50,
    )
    d: dict = {"_raw": out, "ok": "SSH_" not in out[:20]}
    m = re.search(r"^PID=(.*)$", out, re.M)
    d["pid"] = (m.group(1) if m else "").strip()
    m = re.search(r"^BUGS_N=(.*)$", out, re.M)
    d["bugs_n"] = int(re.sub(r"\D", "", (m.group(1) if m else "0")) or 0)
    sm = re.search(r"---SUMMARY---\n(.*?)\n---END_SUMMARY---", out, re.S)
    d["summary_txt"] = sm.group(1).strip() if sm else ""
    # Prefer scp'd summary.json (avoids SSH stream mangling large JSON)
    d["summary"] = None
    tmp_json = Path("/tmp/imx296_day_watchdog/_pull_summary.json")
    if scp_pull(f"{BOARD_OUT}/summary.json", tmp_json, timeout=30):
        try:
            d["summary"] = json.loads(tmp_json.read_text(encoding="utf-8"))
        except Exception:
            d["summary"] = None
    if d["summary"] is None and d["summary_txt"]:
        d["summary"] = _parse_summary_txt(d["summary_txt"])
    bm = re.search(r"---BUGS_TAIL---\n(.*?)\n---END_BUGS---", out, re.S)
    d["bugs_tail"] = bm.group(1).strip() if bm else ""
    rm = re.search(r"---RMODE---\n(.*)", out)
    d["run_mode"] = rm.group(1).strip() if rm else ""
    um = re.search(r"---UNAME---\n(.*)", out)
    d["uname"] = um.group(1).strip() if um else ""
    dm = re.search(r"---DMESG---\n(.*?)\n---END---", out, re.S)
    d["dmesg"] = dm.group(1).strip() if dm else ""
    return d


def restart_board_test() -> str:
    return ssh(
        f"""
set +e
pkill -f '{BOARD_SCRIPT}' 2>/dev/null || true
sleep 1
# free camera
echo free_run > /sys/bus/i2c/devices/4-001a/run_mode 2>/dev/null || true
mkdir -p {BOARD_OUT}
# keep previous bugs
if [ -f {BOARD_OUT}/bugs.jsonl ]; then
  cp {BOARD_OUT}/bugs.jsonl {BOARD_OUT}/bugs.jsonl.bak.$(date +%s) 2>/dev/null || true
fi
nohup /root/dlcv/bin/python3 -u {BOARD_SCRIPT} --hours 24 --out {BOARD_OUT} \
  >>{BOARD_OUT}/console.log 2>&1 &
sleep 2
pgrep -af '{BOARD_SCRIPT}' | head -3
test -f {BOARD_OUT}/summary.txt && head -5 {BOARD_OUT}/summary.txt
""",
        timeout=40,
    )


def ensure_report(path: Path, meta: dict):
    if path.is_file():
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        f"""# IMX296 24h 稳定性连续测试记录

- **开始时间**: {ts()}
- **板卡**: `{HOST}`
- **内核**: `{meta.get('uname', 'unknown')}`
- **测试脚本**: `{BOARD_SCRIPT}` → 输出 `{BOARD_OUT}`
- **主机 watchdog**: `scripts/imx296_day_watchdog.py`
- **关注点**: 模式切换（close/software/line0）、ROI set/reset、底部绿线/串帧、N-1 相位

## 汇总

| 时间 | 状态 | cycles | bugs | green | n1 | mix | 备注 |
|------|------|--------|------|-------|----|-----|------|
""",
        encoding="utf-8",
    )


def append_report(path: Path, d: dict, note: str = ""):
    s = d.get("summary") or {}
    cycles = s.get("cycles", "?")
    bugs = s.get("bugs", d.get("bugs_n", "?"))
    green = s.get("green_frames", "?")
    n1 = s.get("n1_cases", "?")
    mix = s.get("mix_cases", "?")
    pid = d.get("pid") or "-"
    alive = "ALIVE" if pid and pid not in ("",) else "DEAD"
    if not d.get("ok"):
        alive = "SSH_FAIL"
    row = (
        f"| {ts()} | {alive} pid={pid} | {cycles} | {bugs} | {green} | {n1} | {mix} | "
        f"{note.replace('|', '/')} |\n"
    )
    with path.open("a", encoding="utf-8") as f:
        f.write(row)
        if d.get("bugs_tail"):
            f.write(f"\n### {ts()} bugs tail\n\n```\n{d['bugs_tail'][:2000]}\n```\n\n")
        if note.startswith("ALERT") or alive != "ALIVE":
            f.write(f"\n### {ts()} detail\n\n")
            f.write(f"- run_mode: `{d.get('run_mode', '')}`\n")
            if d.get("summary_txt"):
                f.write(f"\n```\n{d['summary_txt'][:1500]}\n```\n\n")
            if d.get("dmesg"):
                f.write(f"\n```\n{d['dmesg'][:1500]}\n```\n\n")


def write_snapshot(snap_dir: Path, d: dict):
    snap_dir.mkdir(parents=True, exist_ok=True)
    (snap_dir / "last_collect.json").write_text(
        json.dumps(
            {
                "ts": ts(),
                "pid": d.get("pid"),
                "bugs_n": d.get("bugs_n"),
                "summary": d.get("summary"),
                "run_mode": d.get("run_mode"),
                "uname": d.get("uname"),
                "bugs_tail": d.get("bugs_tail"),
                "dmesg": d.get("dmesg"),
            },
            indent=2,
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    if d.get("summary_txt"):
        (snap_dir / "summary.txt").write_text(d["summary_txt"] + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--report",
        default=str(Path("/tmp/imx296_day_watchdog/report.md")),
    )
    ap.add_argument("--state-dir", default="/tmp/imx296_day_watchdog")
    ap.add_argument("--interval", type=int, default=120)
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--restart-if-dead", action="store_true", default=True)
    ap.add_argument("--no-restart", action="store_true")
    args = ap.parse_args()

    report = Path(args.report)
    state = Path(args.state_dir)
    state.mkdir(parents=True, exist_ok=True)
    logf = state / "watchdog.log"

    def wlog(msg: str):
        line = f"[{ts()}] {msg}"
        print(line, flush=True)
        with logf.open("a", encoding="utf-8") as f:
            f.write(line + "\n")

    last_bugs = -1
    dead_streak = 0

    while True:
        d = collect()
        write_snapshot(state, d)
        s = d.get("summary") or {}
        bugs = int(s.get("bugs", d.get("bugs_n") or 0) or 0)
        pid = (d.get("pid") or "").strip()
        alive = bool(pid)

        ensure_report(report, d)
        note = ""
        if not d.get("ok"):
            note = "ALERT SSH problem"
            dead_streak += 1
        elif not alive:
            note = "ALERT test process dead"
            dead_streak += 1
        else:
            dead_streak = 0
            if last_bugs >= 0 and bugs > last_bugs:
                note = f"ALERT new bugs +{bugs - last_bugs}"
            else:
                note = "ok"

        append_report(report, d, note=note)
        wlog(
            f"alive={alive} pid={pid or '-'} bugs={bugs} cycles={s.get('cycles')} "
            f"green={s.get('green_frames')} n1={s.get('n1_cases')} note={note}"
        )

        # pull bugs file periodically
        scp_pull(f"{BOARD_OUT}/bugs.jsonl", state / "bugs.jsonl")
        scp_pull(f"{BOARD_OUT}/summary.json", state / "summary.json")

        if (
            (not args.no_restart)
            and args.restart_if_dead
            and dead_streak >= 2
            and d.get("ok")
        ):
            wlog("restarting board endurance")
            out = restart_board_test()
            wlog(out[:500].replace("\n", " | "))
            append_report(report, d, note="ALERT restarted board test")
            dead_streak = 0

        last_bugs = bugs
        if args.once:
            break
        time.sleep(max(30, args.interval))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
