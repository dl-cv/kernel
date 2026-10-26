#!/usr/bin/env python3
"""Exercise extracted production mode handoffs with deterministic I2C/time."""

import argparse
import os
import pathlib
import subprocess
import sys

sys.dont_write_bytecode = True
from test_ar0521_grr import SOURCE, extract_function


FUNCTIONS = (
    "ar0521_update_bits", "ar0521_group_hold", "ar0521_stream_bit",
    "ar0521_restore_slave_backup_locked", "ar0521_enter_slave_locked",
    "ar0521_exit_slave_locked", "ar0521_apply_mode_regs_locked",
    "ar0521_config_grr_seq_trigger_locked", "ar0521_restore_ctrls_for_mode_locked",
    "ar0521_exit_lp11_locked", "ar0521_stream_on_bits", "ar0521_stream_off_bits",
    "ar0521_mode_switch", "ar0521_s_stream",
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, default=SOURCE)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / "tmp").mkdir(exist_ok=True)
    source = args.source.read_text(encoding="utf-8")
    names = list(FUNCTIONS)
    # The same behavioral harness must compile the pre-fix source too.
    if "static int ar0521_wait_standby_locked(" in source:
        names.append("ar0521_wait_standby_locked")
    bodies = [extract_function(source, name) for name in names]
    prototypes = "\n".join(body[:body.index("{")].strip() + ";" for body in bodies)
    mock = pathlib.Path(__file__).with_name("ar0521-mode-mock.c").read_text()
    generated = out / "ar0521-mode-host.c"
    generated.write_text(mock.replace("/* ACTUAL_FUNCTIONS */", prototypes + "\n" + "\n\n".join(bodies)))
    env = dict(os.environ, TMPDIR=str(out / "tmp"), TMP=str(out / "tmp"), TEMP=str(out / "tmp"))
    binary = out / "ar0521-mode-host"
    subprocess.run([args.cc, "-std=gnu11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", str(generated), "-o", str(binary)], check=True, env=env)
    subprocess.run([str(binary)], check=True, env=env)


if __name__ == "__main__":
    main()
