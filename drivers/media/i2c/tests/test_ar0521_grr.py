#!/usr/bin/env python3
"""Compile and exercise the real AR0521 GRR transaction functions on host.

The C mock is deterministic: I2C accesses, frame IRQs, completion waits, and
workqueue joins are injected at explicit points. This tests the state machine
and cleanup ordering only; it does not establish scheduler or hardware
correctness.

Run from Linux/WSL with an isolated output directory:
    python3 test_ar0521_grr.py --out /absolute/.../host-tests --cc /usr/bin/cc
"""

import argparse
import os
import pathlib
import re
import subprocess


SOURCE = pathlib.Path(__file__).resolve().parents[1] / "ar0521.c"
MOCK = pathlib.Path(__file__).with_name("ar0521-grr-mock.c")
FUNCTION_NAMES = (
    "ar0521_grr_frame_irq",
    "ar0521_grr_frame_notify",
    "ar0521_grr_gate",
    "ar0521_grr_trigger_locked",
    "trigger_store",
    "ar0521_quick_stream",
)


def extract_function(source: str, name: str) -> str:
    """Return one complete function body, ignoring braces in comments/strings."""
    match = re.search(
        rf"^static\s+(?:int|void|ssize_t)\s+{re.escape(name)}\s*\(",
        source,
        flags=re.MULTILINE,
    )
    if not match:
        raise RuntimeError(f"missing real function: {name}")

    brace = source.find("{", match.end())
    if brace < 0:
        raise RuntimeError(f"missing function body: {name}")

    depth = 0
    index = brace
    state = "code"
    while index < len(source):
        char = source[index]
        nxt = source[index + 1] if index + 1 < len(source) else ""

        if state == "code":
            if char == "/" and nxt == "*":
                state = "block-comment"
                index += 2
                continue
            if char == "/" and nxt == "/":
                state = "line-comment"
                index += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[match.start():index + 1]
        elif state == "block-comment":
            if char == "*" and nxt == "/":
                state = "code"
                index += 2
                continue
        elif state == "line-comment":
            if char in "\r\n":
                state = "code"
        elif state in ("string", "char"):
            if char == "\\":
                index += 2
                continue
            if (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
        index += 1

    raise RuntimeError(f"unterminated function body: {name}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()

    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=True)
    tmp = output / "tmp"
    tmp.mkdir(parents=True, exist_ok=True)

    source = SOURCE.read_text(encoding="utf-8")
    # Existing sysfs store returns int-or-size_t; preserve production code.
    functions = "\n\n".join(
        ("#pragma GCC diagnostic push\n"
         '#pragma GCC diagnostic ignored "-Wsign-compare"\n'
         + extract_function(source, name) + "\n#pragma GCC diagnostic pop")
        if name == "trigger_store" else extract_function(source, name)
        for name in FUNCTION_NAMES
    )
    scaffold = MOCK.read_text(encoding="utf-8")
    marker = "/* ACTUAL_FUNCTIONS */"
    if scaffold.count(marker) != 1:
        raise RuntimeError("mock must contain exactly one ACTUAL_FUNCTIONS marker")

    generated = output / "ar0521-grr-host.c"
    generated.write_text(
        scaffold.replace(marker, functions),
        encoding="utf-8",
        newline="\n",
    )

    binary = output / "ar0521-grr-host"
    env = os.environ.copy()
    env.update({"TMPDIR": str(tmp), "TMP": str(tmp), "TEMP": str(tmp)})
    subprocess.run(
        [
            args.cc,
            "-std=gnu11",
            "-O0",
            "-g",
            "-pthread",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            str(generated),
            "-o",
            str(binary),
        ],
        check=True,
        cwd=output,
        env=env,
    )
    subprocess.run([str(binary)], check=True, cwd=output, env=env)


if __name__ == "__main__":
    main()