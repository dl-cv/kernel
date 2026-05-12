#!/usr/bin/env python3
"""GPIO0_B7 userspace control tool based on python3-gpiod.

This script targets RK GPIO naming where GPIO0_B7 maps to bank 0 line offset 15.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

try:
    import gpiod  # pyright: ignore[reportMissingImports]
except ImportError as exc:  # pragma: no cover - runtime dependency check
    gpiod = None
    GPIOD_IMPORT_ERROR = exc
else:
    GPIOD_IMPORT_ERROR = None


DEFAULT_CHIP = "gpio0"
DEFAULT_OFFSET = 15
DEFAULT_CONSUMER = "gpio0-b7-python-control"
DEFAULT_LATCH_FILE = "/tmp/gpio0_b7_control_latch.json"


class GpioRuntimeError(RuntimeError):
    """Raised when runtime GPIO environment is invalid."""


def require_gpiod() -> None:
    if gpiod is None:
        raise GpioRuntimeError(
            "python3-gpiod is not installed. "
            "Please install it first (for example: apt install python3-gpiod)."
        ) from GPIOD_IMPORT_ERROR


def level_to_value(level: int):
    return gpiod.line.Value.ACTIVE if level else gpiod.line.Value.INACTIVE


def hold_level(hold_s: float) -> None:
    if hold_s < 0:
        while True:
            time.sleep(1)
    if hold_s > 0:
        time.sleep(hold_s)


def is_process_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def stop_process(pid: int, timeout_s: float = 2.0) -> bool:
    if not is_process_alive(pid):
        return True
    os.kill(pid, signal.SIGTERM)
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if not is_process_alive(pid):
            return True
        time.sleep(0.05)
    os.kill(pid, signal.SIGKILL)
    time.sleep(0.05)
    return not is_process_alive(pid)


def list_gpiochips() -> list[tuple[Path, object]]:
    require_gpiod()
    chips: list[tuple[Path, object]] = []
    for chip_path in sorted(Path("/dev").glob("gpiochip*")):
        try:
            chip = gpiod.Chip(str(chip_path))
        except OSError:
            continue
        try:
            info = chip.get_info()
            chips.append((chip_path, info))
        finally:
            chip.close()
    return chips


def resolve_chip_path(chip_selector: str) -> str:
    require_gpiod()
    selector = chip_selector.strip()
    if selector.startswith("/dev/gpiochip"):
        return selector
    if selector.startswith("gpiochip"):
        candidate = f"/dev/{selector}"
        if Path(candidate).exists():
            return candidate

    chips = list_gpiochips()
    for chip_path, info in chips:
        if selector in {getattr(info, "name", ""), getattr(info, "label", "")}:
            return str(chip_path)

    if chips:
        available = ", ".join(
            f"{path.name}(name={getattr(info, 'name', '-')},label={getattr(info, 'label', '-')})"
            for path, info in chips
        )
    else:
        available = "no gpiochip device found under /dev"
    raise GpioRuntimeError(
        f"Cannot resolve chip selector '{chip_selector}'. Available: {available}"
    )


def request_output_line(
    chip_path: str,
    offset: int,
    initial_level: int,
    consumer: str,
):
    require_gpiod()
    settings = gpiod.LineSettings()
    settings.direction = gpiod.line.Direction.OUTPUT
    settings.output_value = level_to_value(initial_level)
    return gpiod.request_lines(
        chip_path,
        consumer=consumer,
        config={offset: settings},
    )


@dataclass(frozen=True)
class PulseSpec:
    idle_level: int
    active_level: int
    pulse_width_s: float
    count: int
    interval_s: float

    def validate(self) -> None:
        if self.idle_level not in (0, 1):
            raise ValueError("idle_level must be 0 or 1")
        if self.active_level not in (0, 1):
            raise ValueError("active_level must be 0 or 1")
        if self.idle_level == self.active_level:
            raise ValueError("active_level must be opposite to idle_level")
        if self.count < 1:
            raise ValueError("count must be >= 1")
        if self.pulse_width_s <= 0:
            raise ValueError("pulse_width_s must be > 0")
        if self.interval_s < 0:
            raise ValueError("interval_s must be >= 0")


@dataclass
class LatchInfo:
    pid: int
    chip_path: str
    offset: int
    level: int
    consumer: str

    @classmethod
    def from_dict(cls, data: dict) -> "LatchInfo":
        return cls(
            pid=int(data["pid"]),
            chip_path=str(data["chip_path"]),
            offset=int(data["offset"]),
            level=int(data["level"]),
            consumer=str(data["consumer"]),
        )

    def to_dict(self) -> dict:
        return {
            "pid": self.pid,
            "chip_path": self.chip_path,
            "offset": self.offset,
            "level": self.level,
            "consumer": self.consumer,
        }


def read_latch_info(latch_file: Path) -> LatchInfo | None:
    if not latch_file.exists():
        return None
    try:
        data = json.loads(latch_file.read_text(encoding="utf-8"))
        return LatchInfo.from_dict(data)
    except (json.JSONDecodeError, OSError, KeyError, TypeError, ValueError):
        return None


def write_latch_info(latch_file: Path, info: LatchInfo) -> None:
    latch_file.write_text(
        json.dumps(info.to_dict(), ensure_ascii=True, indent=2) + "\n",
        encoding="utf-8",
    )


def clear_latch_file(latch_file: Path) -> None:
    if latch_file.exists():
        latch_file.unlink()


class GPIOController:
    def __init__(self, chip_path: str, offset: int, consumer: str):
        self.chip_path = chip_path
        self.offset = offset
        self.consumer = consumer

    def inspect(self) -> str:
        require_gpiod()
        chip = gpiod.Chip(self.chip_path)
        try:
            chip_info = chip.get_info()
            line_info = chip.get_line_info(self.offset)
        finally:
            chip.close()

        return "\n".join(
            [
                f"chip_path: {self.chip_path}",
                f"chip_name: {getattr(chip_info, 'name', '-')}",
                f"chip_label: {getattr(chip_info, 'label', '-')}",
                f"chip_num_lines: {getattr(chip_info, 'num_lines', '-')}",
                f"line_offset: {self.offset}",
                f"line_name: {getattr(line_info, 'name', '-')}",
                f"line_used: {getattr(line_info, 'used', '-')}",
                f"line_consumer: {getattr(line_info, 'consumer', '-')}",
                f"line_direction: {getattr(line_info, 'direction', '-')}",
                f"line_active_low: {getattr(line_info, 'active_low', '-')}",
            ]
        )

    def set_level(self, level: int, hold_s: float = 0.0) -> None:
        req = request_output_line(self.chip_path, self.offset, level, self.consumer)
        try:
            req.set_value(self.offset, level_to_value(level))
            hold_level(hold_s)
        finally:
            req.release()

    def pulse(self, spec: PulseSpec) -> None:
        spec.validate()
        req = request_output_line(
            self.chip_path,
            self.offset,
            spec.idle_level,
            self.consumer,
        )
        try:
            for pulse_idx in range(spec.count):
                req.set_value(self.offset, level_to_value(spec.active_level))
                time.sleep(spec.pulse_width_s)
                req.set_value(self.offset, level_to_value(spec.idle_level))
                if pulse_idx < spec.count - 1 and spec.interval_s > 0:
                    time.sleep(spec.interval_s)
        finally:
            req.release()

    def pulse_high(self, count: int, pulse_width_s: float, interval_s: float) -> None:
        self.pulse(
            PulseSpec(
                idle_level=0,
                active_level=1,
                pulse_width_s=pulse_width_s,
                count=count,
                interval_s=interval_s,
            )
        )

    def pulse_low(self, count: int, pulse_width_s: float, interval_s: float) -> None:
        self.pulse(
            PulseSpec(
                idle_level=1,
                active_level=0,
                pulse_width_s=pulse_width_s,
                count=count,
                interval_s=interval_s,
            )
        )


def start_latch_daemon(
    *,
    chip_path: str,
    offset: int,
    level: int,
    consumer: str,
    latch_file: Path,
) -> None:
    stop_latch_daemon(latch_file=latch_file, quiet=True)
    cmd = [
        sys.executable,
        str(Path(__file__).resolve()),
        "--chip",
        chip_path,
        "--offset",
        str(offset),
        "--consumer",
        consumer,
        "--latch-file",
        str(latch_file),
        "high" if level else "low",
        "--hold-s",
        "-1",
    ]
    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    info = LatchInfo(
        pid=proc.pid,
        chip_path=chip_path,
        offset=offset,
        level=level,
        consumer=consumer,
    )
    write_latch_info(latch_file, info)
    time.sleep(0.1)
    if not is_process_alive(proc.pid):
        clear_latch_file(latch_file)
        raise GpioRuntimeError(
            "failed to start latch daemon, check line ownership and permissions"
        )


def stop_latch_daemon(*, latch_file: Path, quiet: bool = False) -> bool:
    info = read_latch_info(latch_file)
    if info is None:
        if not quiet:
            print(f"latch: not running (no state file: {latch_file})")
        clear_latch_file(latch_file)
        return False

    if stop_process(info.pid):
        if not quiet:
            print(f"latch: stopped pid={info.pid}")
    else:
        raise GpioRuntimeError(f"failed to stop latch pid={info.pid}")
    clear_latch_file(latch_file)
    return True


def print_latch_status(*, latch_file: Path) -> None:
    info = read_latch_info(latch_file)
    if info is None:
        print(f"latch: stopped (no state file: {latch_file})")
        return

    alive = is_process_alive(info.pid)
    print(
        "\n".join(
            [
                f"latch_file: {latch_file}",
                f"running: {alive}",
                f"pid: {info.pid}",
                f"chip_path: {info.chip_path}",
                f"offset: {info.offset}",
                f"level: {info.level}",
                f"consumer: {info.consumer}",
            ]
        )
    )
    if not alive:
        print("warning: stale latch state file detected, run `latch-stop` to clean it.")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Control GPIO0_B7 high/low/pulse via python3-gpiod."
    )
    parser.add_argument(
        "--chip",
        default=DEFAULT_CHIP,
        help="gpiochip selector: gpio0/gpiochipN/or /dev/gpiochipN",
    )
    parser.add_argument(
        "--offset",
        type=int,
        default=DEFAULT_OFFSET,
        help="line offset inside the selected chip (GPIO0_B7 is usually 15)",
    )
    parser.add_argument(
        "--consumer",
        default=DEFAULT_CONSUMER,
        help="consumer name shown in gpioinfo",
    )
    parser.add_argument(
        "--latch-file",
        default=DEFAULT_LATCH_FILE,
        help="state file for background latch mode",
    )

    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("inspect", help="print chip and line ownership info")

    parser_high = sub.add_parser("high", help="set output high")
    parser_high.add_argument(
        "--hold-s",
        type=float,
        default=0.0,
        help="how long to hold level before releasing line, <0 means forever",
    )

    parser_low = sub.add_parser("low", help="set output low")
    parser_low.add_argument(
        "--hold-s",
        type=float,
        default=0.0,
        help="how long to hold level before releasing line, <0 means forever",
    )

    parser_pulse_high = sub.add_parser(
        "pulse-high", help="idle low then output high pulse(s)"
    )
    parser_pulse_high.add_argument("--count", type=int, default=1)
    parser_pulse_high.add_argument("--pulse-width-s", type=float, default=0.05)
    parser_pulse_high.add_argument("--interval-s", type=float, default=0.05)

    parser_pulse_low = sub.add_parser(
        "pulse-low", help="idle high then output low pulse(s)"
    )
    parser_pulse_low.add_argument("--count", type=int, default=1)
    parser_pulse_low.add_argument("--pulse-width-s", type=float, default=0.05)
    parser_pulse_low.add_argument("--interval-s", type=float, default=0.05)

    parser_pulse = sub.add_parser("pulse", help="generic pulse with full parameters")
    parser_pulse.add_argument("--idle-level", type=int, choices=[0, 1], required=True)
    parser_pulse.add_argument(
        "--active-level", type=int, choices=[0, 1], required=True
    )
    parser_pulse.add_argument("--count", type=int, default=1)
    parser_pulse.add_argument("--pulse-width-s", type=float, default=0.05)
    parser_pulse.add_argument("--interval-s", type=float, default=0.05)

    sub.add_parser(
        "latch-high",
        help="start background process and keep output high until changed",
    )

    sub.add_parser(
        "latch-low",
        help="start background process and keep output low until changed",
    )

    sub.add_parser("latch-stop", help="stop background latch process")
    sub.add_parser("latch-status", help="show background latch process status")

    return parser


def run(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        latch_file = Path(args.latch_file)
        if args.command == "latch-stop":
            stop_latch_daemon(latch_file=latch_file)
            return 0
        if args.command == "latch-status":
            print_latch_status(latch_file=latch_file)
            return 0

        chip_path = resolve_chip_path(args.chip)
        controller = GPIOController(
            chip_path=chip_path,
            offset=args.offset,
            consumer=args.consumer,
        )

        if args.command == "inspect":
            print(controller.inspect())
            return 0
        if args.command == "high":
            controller.set_level(level=1, hold_s=args.hold_s)
            return 0
        if args.command == "low":
            controller.set_level(level=0, hold_s=args.hold_s)
            return 0
        if args.command == "pulse-high":
            controller.pulse_high(
                count=args.count,
                pulse_width_s=args.pulse_width_s,
                interval_s=args.interval_s,
            )
            return 0
        if args.command == "pulse-low":
            controller.pulse_low(
                count=args.count,
                pulse_width_s=args.pulse_width_s,
                interval_s=args.interval_s,
            )
            return 0
        if args.command == "pulse":
            controller.pulse(
                PulseSpec(
                    idle_level=args.idle_level,
                    active_level=args.active_level,
                    pulse_width_s=args.pulse_width_s,
                    count=args.count,
                    interval_s=args.interval_s,
                )
            )
            return 0
        if args.command == "latch-high":
            start_latch_daemon(
                chip_path=chip_path,
                offset=args.offset,
                level=1,
                consumer=args.consumer,
                latch_file=latch_file,
            )
            print(
                f"latch: high level is being held in background, use latch-status/latch-stop (state file: {latch_file})"
            )
            return 0
        if args.command == "latch-low":
            start_latch_daemon(
                chip_path=chip_path,
                offset=args.offset,
                level=0,
                consumer=args.consumer,
                latch_file=latch_file,
            )
            print(
                f"latch: low level is being held in background, use latch-status/latch-stop (state file: {latch_file})"
            )
            return 0
        parser.error(f"Unknown command: {args.command}")
        return 2
    except (GpioRuntimeError, ValueError, OSError) as err:
        print(f"[gpio0_b7_control] {err}", file=sys.stderr)
        return 1


def main() -> int:
    return run(sys.argv[1:])


if __name__ == "__main__":
    raise SystemExit(main())
