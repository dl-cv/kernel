#!/usr/bin/env python3
"""Exercise the real AR0521 ERS single-shot state machine on host."""
import argparse
import pathlib
import subprocess
import sys

sys.dont_write_bytecode = True
from test_ar0521_grr import SOURCE, extract_function

FUNCTIONS = (
    "ar0521_wait_standby_locked",
    "ar0521_config_ers_locked",
    "ar0521_apply_mode_regs_locked",
    "ar0521_prepare_mode_stream_locked",
    "ar0521_apply_exposure_locked",
    "ar0521_update_exposure_range_locked",
    "ar0521_ers_frame_irq",
    "ar0521_ers_stream_off_work",
    "ar0521_ers_trigger_locked",
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--source", type=pathlib.Path, default=SOURCE)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = args.source.read_text(encoding="utf-8")
    bodies = [extract_function(source, name) for name in FUNCTIONS]
    prototypes = "\n".join(body[:body.index("{")].strip() + ";" for body in bodies)
    mock = pathlib.Path(__file__).with_name("ar0521-ers-mock.c").read_text(encoding="utf-8")
    generated = out / "ar0521-ers-host.c"
    generated.write_text(mock.replace("/* ACTUAL_FUNCTIONS */", prototypes + "\n" + "\n\n".join(bodies)), encoding="utf-8")
    binary = out / "ar0521-ers-host"
    subprocess.run([
        args.cc, "-std=gnu11", "-O0", "-g", "-pthread", "-Wall", "-Wextra",
        "-Werror", "-Wno-unused-parameter", str(generated), "-o", str(binary),
    ], check=True, cwd=out)
    subprocess.run([str(binary)], check=True, cwd=out)


if __name__ == "__main__":
    main()
