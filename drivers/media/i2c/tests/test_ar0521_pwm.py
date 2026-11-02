#!/usr/bin/env python3
"""Run production PWM/slave helpers with pin-level and cached-state mocks."""
import argparse
import pathlib
import subprocess
import sys

sys.dont_write_bytecode = True
from test_ar0521_grr import SOURCE, extract_function

FUNCTIONS = (
    "ar0521_pwm_period_ns", "ar0521_trigger_period_ns",
    "ar0521_init_trigger_pwm", "ar0521_update_bits",
    "ar0521_restore_slave_backup_locked", "ar0521_enter_slave_locked",
    "ar0521_trigger_once_locked",
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, default=SOURCE)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    source = args.source.read_text(encoding="utf-8")
    bodies = [extract_function(source, name) for name in FUNCTIONS]
    prototypes = "\n".join(body[:body.index("{")].strip() + ";" for body in bodies)
    mock = pathlib.Path(__file__).with_name("ar0521-pwm-mock.c").read_text(encoding="utf-8")
    generated = args.out / "ar0521-pwm-host.c"
    generated.write_text(mock.replace("/* ACTUAL_FUNCTIONS */", prototypes + "\n" + "\n\n".join(bodies)), encoding="utf-8")
    msvc = pathlib.Path(args.cc).name.lower() in ("cl", "cl.exe")
    binary = args.out / ("ar0521-pwm-host.exe" if msvc else "ar0521-pwm-host")
    if msvc:
        command = [args.cc, "/nologo", "/TC", "/std:c11", "/W4", "/WX",
                   "/wd4100", "/wd4505", str(generated),
                   "/Fe:" + str(binary), "/Fo:" + str(args.out / "pwm.obj")]
    else:
        command = [args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-parameter", str(generated), "-o", str(binary)]
    subprocess.run(command, check=True, cwd=args.out)
    subprocess.run([str(binary)], check=True, cwd=args.out, timeout=15)


if __name__ == "__main__":
    main()
