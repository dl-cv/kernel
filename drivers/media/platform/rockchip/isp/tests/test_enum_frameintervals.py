#!/usr/bin/env python3
"""Host regression for rkisp_enum_frameintervals() and the subdev pad wrapper."""
import argparse
import os
import pathlib
import re
import subprocess


def extract_function(source: str, name: str) -> str:
    match = re.search(rf"^static int {re.escape(name)}\s*\(", source, re.MULTILINE)
    if not match:
        raise RuntimeError(f"missing production function: {name}")

    brace = source.find("{", match.end())
    depth = 0
    state = "code"
    index = brace
    while index < len(source):
        char = source[index]
        nxt = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and nxt == "*":
                state = "comment"
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
        elif state == "comment":
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
    raise RuntimeError(f"unterminated production function: {name}")


SCAFFOLD = r'''#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#define ENOIOCTLCMD 515
struct file { int unused; };
struct fake_subdev {
    uint32_t num_pads;
    int (*g_frame_interval)(struct v4l2_subdev_frame_interval *);
};
struct rkisp_sensor_info { struct fake_subdev *sd; };
struct rkisp_device { struct rkisp_sensor_info *active_sensor; };
struct rkisp_stream { struct rkisp_device *ispdev; };
static struct rkisp_stream *test_stream;
static struct rkisp_stream *video_drvdata(struct file *file)
{
    (void)file;
    return test_stream;
}
static int wrapper_calls;
static int getter_calls;
static int getter_result;
static int check_pad(struct fake_subdev *sd, uint32_t pad)
{
    if (sd->num_pads) {
        if (pad >= sd->num_pads)
            return -EINVAL;
        return 0;
    }
    return pad > 0 ? -EINVAL : 0;
}
static int call_g_frame_interval(struct fake_subdev *sd,
                                 struct v4l2_subdev_frame_interval *fi)
{
    int ret = check_pad(sd, fi->pad);
    wrapper_calls++;
    return ret ? ret : sd->g_frame_interval(fi);
}
#define v4l2_subdev_call(sd, group, op, fi) call_g_frame_interval((sd), (fi))
#define v4l2_err(...) ((void)0)
static int test_get_frame_interval(struct v4l2_subdev_frame_interval *fi)
{
    getter_calls++;
    if (fi->pad != 0)
        return -EINVAL;
    if (getter_result)
        return getter_result;
    fi->interval.numerator = 1001;
    fi->interval.denominator = 30000;
    return 0;
}
'''

MAIN = r'''
static int failures;
static void expect_result(const char *name, int actual, int expected)
{
    if (actual != expected) {
        fprintf(stderr, "FAIL %s: got %d, expected %d\n", name, actual, expected);
        failures++;
    } else {
        printf("PASS %s: %d\n", name, actual);
    }
}
static void expect_interval_fields(const char *name,
                                  const struct v4l2_frmivalenum *fival,
                                  uint32_t min_num, uint32_t min_den)
{
    if (fival->type != V4L2_FRMIVAL_TYPE_CONTINUOUS ||
        fival->stepwise.step.numerator != 1 || fival->stepwise.step.denominator != 1 ||
        fival->stepwise.max.numerator != 1 || fival->stepwise.max.denominator != 1 ||
        fival->stepwise.min.numerator != min_num ||
        fival->stepwise.min.denominator != min_den) {
        fprintf(stderr, "FAIL %s: type=%u step=%u/%u max=%u/%u min=%u/%u\n",
                name, fival->type,
                fival->stepwise.step.numerator, fival->stepwise.step.denominator,
                fival->stepwise.max.numerator, fival->stepwise.max.denominator,
                fival->stepwise.min.numerator, fival->stepwise.min.denominator);
        failures++;
    } else {
        printf("PASS %s fields (type=%u step=1/1 max=1/1 min=%u/%u)\n",
               name, fival->type, min_num, min_den);
    }
}
static void reset_fixture(struct rkisp_stream *stream,
                          struct rkisp_device *dev,
                          struct rkisp_sensor_info *sensor,
                          struct fake_subdev *sd,
                          struct v4l2_frmivalenum *fival)
{
    *sd = (struct fake_subdev) { .num_pads = 1, .g_frame_interval = test_get_frame_interval };
    *sensor = (struct rkisp_sensor_info) { .sd = sd };
    *dev = (struct rkisp_device) { .active_sensor = sensor };
    *stream = (struct rkisp_stream) { .ispdev = dev };
    *fival = (struct v4l2_frmivalenum) {0};
    test_stream = stream;
    wrapper_calls = 0;
    getter_calls = 0;
    getter_result = 0;
}
int main(void)
{
    struct rkisp_stream stream;
    struct rkisp_device dev;
    struct rkisp_sensor_info sensor;
    struct fake_subdev sd;
    struct v4l2_frmivalenum fival;
    struct file file = {0};
    int ret;

    reset_fixture(&stream, &dev, &sensor, &sd, &fival);
    ret = rkisp_enum_frameintervals(&file, NULL, &fival);
    expect_result("valid one-pad sensor call", ret, 0);
    if (ret == 0) {
        if (wrapper_calls != 1 || getter_calls != 1) {
            fprintf(stderr, "FAIL wrapper/getter call counts\n");
            failures++;
        }
        expect_interval_fields("sensor interval", &fival, 1001, 30000);
    }

    reset_fixture(&stream, &dev, &sensor, &sd, &fival);
    fival.index = 1;
    ret = rkisp_enum_frameintervals(&file, NULL, &fival);
    expect_result("nonzero index", ret, -EINVAL);
    if (wrapper_calls || getter_calls) failures++;

    reset_fixture(&stream, &dev, &sensor, &sd, &fival);
    dev.active_sensor = NULL;
    ret = rkisp_enum_frameintervals(&file, NULL, &fival);
    expect_result("missing active sensor", ret, -ENODEV);
    if (wrapper_calls || getter_calls) failures++;

    reset_fixture(&stream, &dev, &sensor, &sd, &fival);
    getter_result = -EIO;
    ret = rkisp_enum_frameintervals(&file, NULL, &fival);
    expect_result("getter error propagation", ret, -EIO);

    reset_fixture(&stream, &dev, &sensor, &sd, &fival);
    getter_result = -ENOIOCTLCMD;
    ret = rkisp_enum_frameintervals(&file, NULL, &fival);
    expect_result("unsupported getter fallback", ret, 0);
    if (ret == 0)
        expect_interval_fields("30 fps fallback", &fival, 1, 30);

    return failures ? 1 : 0;
}
'''

parser = argparse.ArgumentParser()
parser.add_argument("--source", type=pathlib.Path, required=True,
                    help="capture.c source to test; may be a saved git baseline copy")
parser.add_argument("--out", type=pathlib.Path, required=True,
                    help="ignored directory for generated host source, binary, and log")
parser.add_argument("--cc", default="gcc")
args = parser.parse_args()
source_path = args.source.resolve()
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
production = extract_function(source_path.read_text(encoding="utf-8-sig"),
                              "rkisp_enum_frameintervals")
host_c = out / "rkisp-enum-frameintervals-host.c"
binary = out / "rkisp-enum-frameintervals-host"
log = out / "rkisp-enum-frameintervals-host.log"
host_c.write_text(SCAFFOLD + production + MAIN, encoding="utf-8", newline="\n")
env = os.environ.copy()
env.update({"TMPDIR": str(out), "TMP": str(out), "TEMP": str(out)})
with log.open("w", encoding="utf-8") as output:
    subprocess.run([args.cc, "-std=gnu11", "-O0", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-ftrivial-auto-var-init=pattern",
                    str(host_c), "-o", str(binary)], check=True, cwd=out,
                   env=env, stdout=output, stderr=subprocess.STDOUT)
    result = subprocess.run([str(binary)], cwd=out, env=env,
                            stdout=output, stderr=subprocess.STDOUT)
print(log.read_text(encoding="utf-8"), end="")
raise SystemExit(result.returncode)
