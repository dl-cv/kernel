#!/usr/bin/env python3
"""Compile/exercise real trigger-dev gate, IRQ, and debounce functions."""
import argparse
import pathlib
import subprocess
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3] / "media/i2c/tests"))
from test_ar0521_grr import extract_function
SOURCE = pathlib.Path(__file__).resolve().parents[1] / "trigger-dev.c"
MOCK = pathlib.Path(__file__).with_name("trigger-dev-gate-mock.c")
def extract_irq_handler(source):
    start = source.index("static irqreturn_t trigger_dev_irq")
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start:end + 1]
    raise ValueError("unterminated function: trigger_dev_irq")
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, default=SOURCE)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    source = args.source.read_text(encoding="utf-8")
    functions = "\n".join((
        extract_function(source, "trigger_dev_set_enabled"),
        extract_function(source, "trigger_dev_debounce_work"),
        extract_irq_handler(source),
    ))
    generated = args.out / "trigger-dev-gate-host.c"
    generated.write_text(MOCK.read_text(encoding="utf-8").replace(
        "/* ACTUAL_FUNCTIONS */", functions), encoding="utf-8")
    binary = args.out / "trigger-dev-gate-host"
    subprocess.run([args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-pthread", str(generated),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
if __name__ == "__main__":
    main()
