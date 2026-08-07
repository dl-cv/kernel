#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
DLCVCAM boot.img (FIT) integrity checker — hash only, no signature.

Verifies:
  1) Optional whole-file SHA-256 against <img>.sha256 or --expect-sha256
  2) FIT/FDT structure parse
  3) Embedded per-image hash nodes (fdt / kernel / resource, ...)

Exit codes:
  0  OK
  1  verification failed
  2  usage / I/O / parse error
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_NOP = 4
FDT_END = 9

SUPPORTED_HASH = {
    "sha1": (hashlib.sha1, 20),
    "sha256": (hashlib.sha256, 32),
    "sha512": (hashlib.sha512, 64),
}


class VerifyError(Exception):
    pass


def eprint(*args: object) -> None:
    print(*args, file=sys.stderr)


def sha256_file(path: Path, chunk: int = 1024 * 1024) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def parse_sha256_sidecar(text: str) -> str:
    """
    Accept:
      <hex>
      <hex>  <filename>
      <hex> *<filename>
    """
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        part = line.split()[0].strip().lower()
        if len(part) == 64 and all(c in "0123456789abcdef" for c in part):
            return part
    raise VerifyError("sidecar 中未找到合法 sha256")


def align4(n: int) -> int:
    return (n + 3) & ~3


def parse_fdt_tree(data: bytes, fdt_offset: int = 0) -> Dict[str, Dict[str, bytes]]:
    if len(data) < fdt_offset + 40:
        raise VerifyError("文件过短，无法读取 FDT 头")

    magic, totalsize, off_dt_struct, off_dt_strings, _off_mem, version, _last, _cpu, size_dt_strings, size_dt_struct = struct.unpack_from(
        ">10I", data, fdt_offset
    )
    if magic != FDT_MAGIC:
        raise VerifyError(f"非 FIT/FDT 魔数: 0x{magic:08x} (期望 0xd00dfeed)")
    if version < 16:
        raise VerifyError(f"不支持的 FDT version: {version}")
    if totalsize < 40 or fdt_offset + totalsize > len(data):
        # external-data FIT: totalsize 只覆盖 header tree，payload 在后面 —— 允许 totalsize 小于文件
        if totalsize < 40:
            raise VerifyError(f"非法 FDT totalsize: {totalsize}")

    struct_base = fdt_offset + off_dt_struct
    strings_base = fdt_offset + off_dt_strings
    struct_end = struct_base + size_dt_struct
    strings_end = strings_base + size_dt_strings
    if struct_end > len(data) or strings_end > len(data):
        raise VerifyError("FDT struct/strings 超出文件范围")

    def get_string(nameoff: int) -> str:
        start = strings_base + nameoff
        if start >= strings_end:
            raise VerifyError("FDT 字符串偏移越界")
        end = data.find(b"\x00", start, strings_end)
        if end < 0:
            raise VerifyError("FDT 字符串缺少 NUL")
        return data[start:end].decode("ascii", "replace")

    pos = struct_base
    depth = 0
    stack: List[str] = []
    props: Dict[str, Dict[str, bytes]] = {}

    while pos + 4 <= struct_end:
        tag = struct.unpack_from(">I", data, pos)[0]
        pos += 4
        if tag == FDT_BEGIN_NODE:
            end = data.find(b"\x00", pos, struct_end)
            if end < 0:
                raise VerifyError("FDT 节点名缺少 NUL")
            name = data[pos:end].decode("ascii", "replace")
            pos = align4(end + 1)
            depth += 1
            if depth == 1:
                full = "/"
            else:
                parent = stack[-1]
                full = f"{parent}{name}" if parent.endswith("/") else f"{parent}/{name}"
                if parent == "/":
                    full = f"/{name}"
            stack.append(full)
            props.setdefault(full, {})
        elif tag == FDT_END_NODE:
            if not stack:
                raise VerifyError("FDT END_NODE 不匹配")
            stack.pop()
            depth -= 1
        elif tag == FDT_PROP:
            if pos + 8 > struct_end:
                raise VerifyError("FDT PROP 头截断")
            plen, nameoff = struct.unpack_from(">II", data, pos)
            pos += 8
            if pos + plen > struct_end:
                raise VerifyError("FDT PROP 值截断")
            pname = get_string(nameoff)
            pval = data[pos : pos + plen]
            pos = align4(pos + plen)
            if not stack:
                raise VerifyError("FDT PROP 无当前节点")
            props[stack[-1]][pname] = pval
        elif tag == FDT_NOP:
            continue
        elif tag == FDT_END:
            break
        else:
            raise VerifyError(f"未知 FDT tag {tag} @ {pos - 4}")

    return props


def _cstr(val: bytes) -> str:
    return val.split(b"\x00", 1)[0].decode("ascii", "replace")


def _u32(val: bytes) -> Optional[int]:
    if len(val) != 4:
        return None
    return struct.unpack(">I", val)[0]


def collect_fit_images(props: Dict[str, Dict[str, bytes]]) -> List[Dict[str, Any]]:
    images: List[Dict[str, Any]] = []
    for path, p in props.items():
        # image nodes: /images/<name>  (exactly one name segment under /images)
        if not path.startswith("/images/"):
            continue
        rest = path[len("/images/") :]
        if not rest or "/" in rest:
            continue
        name = rest
        img: Dict[str, Any] = {
            "name": name,
            "path": path,
            "type": _cstr(p["type"]) if "type" in p else "",
            "compression": _cstr(p["compression"]) if "compression" in p else "",
            "data_position": _u32(p["data-position"]) if "data-position" in p else None,
            "data_offset": _u32(p["data-offset"]) if "data-offset" in p else None,
            "data_size": _u32(p["data-size"]) if "data-size" in p else None,
            "hashes": [],
        }
        # inline data property (rare for our external FIT)
        if "data" in p:
            img["inline_data_len"] = len(p["data"])
            img["inline_data"] = p["data"]

        # hash subnodes: /images/<name>/hash or hash@1 ...
        prefix = path + "/"
        for hpath, hp in props.items():
            if not hpath.startswith(prefix):
                continue
            sub = hpath[len(prefix) :]
            if "/" in sub:
                continue
            if not (sub == "hash" or sub.startswith("hash@") or sub.startswith("hash-")):
                continue
            if "algo" not in hp or "value" not in hp:
                continue
            img["hashes"].append(
                {
                    "node": sub,
                    "algo": _cstr(hp["algo"]).lower(),
                    "value": hp["value"],
                }
            )
        images.append(img)
    images.sort(key=lambda x: x["name"])
    return images


def resolve_image_blob(data: bytes, img: Dict[str, Any], fdt_totalsize_hint: int) -> bytes:
    if "inline_data" in img:
        return img["inline_data"]

    size = img.get("data_size")
    if size is None:
        raise VerifyError(f"镜像 {img['name']}: 缺少 data-size/data")

    pos = img.get("data_position")
    if pos is not None:
        end = pos + size
        if end > len(data):
            raise VerifyError(
                f"镜像 {img['name']}: data-position/size 越界 ({pos}+{size} > {len(data)})"
            )
        return data[pos:end]

    off = img.get("data_offset")
    if off is not None:
        # external data: offset relative to end of FIT header (aligned external area)
        # U-Boot -E external data: payload starts after FIT totalsize, often aligned.
        # data-offset is relative to the external data base (right after FIT).
        base = fdt_totalsize_hint
        # mkimage -E -p 0x800: external data base is max(totalsize aligned, -p alignment)
        # For our images, data-position is absolute; data-offset path kept for completeness.
        pos2 = base + off
        end = pos2 + size
        if end > len(data):
            raise VerifyError(
                f"镜像 {img['name']}: data-offset/size 越界 ({pos2}+{size} > {len(data)})"
            )
        return data[pos2:end]

    raise VerifyError(f"镜像 {img['name']}: 无 data / data-position / data-offset")


def verify_fit_hashes(data: bytes) -> Tuple[bool, List[str], List[Dict[str, Any]]]:
    props = parse_fdt_tree(data, 0)
    magic, totalsize = struct.unpack_from(">2I", data, 0)
    assert magic == FDT_MAGIC

    images = collect_fit_images(props)
    if not images:
        raise VerifyError("FIT 中未找到 /images/* 节点")

    messages: List[str] = []
    details: List[Dict[str, Any]] = []
    all_ok = True

    for img in images:
        entry: Dict[str, Any] = {
            "name": img["name"],
            "type": img["type"],
            "data_size": img.get("data_size"),
            "data_position": img.get("data_position"),
            "hashes": [],
        }
        try:
            blob = resolve_image_blob(data, img, totalsize)
        except VerifyError as ex:
            all_ok = False
            messages.append(f"[FAIL] {img['name']}: {ex}")
            entry["error"] = str(ex)
            details.append(entry)
            continue

        if not img["hashes"]:
            all_ok = False
            messages.append(f"[FAIL] {img['name']}: 无内嵌 hash 节点")
            entry["error"] = "no hash node"
            details.append(entry)
            continue

        for h in img["hashes"]:
            algo = h["algo"]
            val = h["value"]
            hinfo: Dict[str, Any] = {
                "node": h["node"],
                "algo": algo,
                "expected": val.hex(),
            }
            if algo not in SUPPORTED_HASH:
                all_ok = False
                hinfo["ok"] = False
                hinfo["error"] = f"不支持的 hash 算法: {algo}"
                messages.append(f"[FAIL] {img['name']}/{h['node']}: 不支持算法 {algo}")
                entry["hashes"].append(hinfo)
                continue
            factory, digest_len = SUPPORTED_HASH[algo]
            if len(val) != digest_len:
                # some FIT blobs store value with padding
                if len(val) > digest_len and all(b == 0 for b in val[digest_len:]):
                    val = val[:digest_len]
                    hinfo["expected"] = val.hex()
                else:
                    all_ok = False
                    hinfo["ok"] = False
                    hinfo["error"] = f"hash 长度 {len(val)} != {digest_len}"
                    messages.append(
                        f"[FAIL] {img['name']}/{h['node']}: hash 长度异常 {len(val)}"
                    )
                    entry["hashes"].append(hinfo)
                    continue
            actual = factory(blob).digest()
            hinfo["actual"] = actual.hex()
            ok = hmac_compare(actual, val)
            hinfo["ok"] = ok
            if ok:
                messages.append(
                    f"[OK]   {img['name']}/{h['node']} {algo} ({len(blob)} bytes)"
                )
            else:
                all_ok = False
                messages.append(
                    f"[FAIL] {img['name']}/{h['node']} {algo} mismatch\n"
                    f"       expect {val.hex()}\n"
                    f"       actual {actual.hex()}"
                )
            entry["hashes"].append(hinfo)
        details.append(entry)

    return all_ok, messages, details


def hmac_compare(a: bytes, b: bytes) -> bool:
    if len(a) != len(b):
        return False
    diff = 0
    for x, y in zip(a, b):
        diff |= x ^ y
    return diff == 0


def load_expect_sha256(img: Path, explicit: Optional[str], sidecar: Optional[Path]) -> Optional[str]:
    if explicit:
        v = explicit.strip().lower()
        if len(v) != 64 or any(c not in "0123456789abcdef" for c in v):
            raise VerifyError("--expect-sha256 不是 64 位 hex")
        return v
    candidates: List[Path] = []
    if sidecar:
        candidates.append(sidecar)
    candidates.append(Path(str(img) + ".sha256"))
    candidates.append(img.with_suffix(img.suffix + ".sha256"))
    # also boot-xxx.img.sha256 already covered; try stem.sha256
    candidates.append(img.with_suffix(".sha256"))
    for c in candidates:
        if c.is_file():
            return parse_sha256_sidecar(c.read_text(encoding="utf-8", errors="replace"))
    return None


def write_manifest(img: Path, file_sha: str, fit_details: List[Dict[str, Any]], meta: Dict[str, Any]) -> Path:
    out = Path(str(img) + ".dlcvcam.json")
    doc = {
        "format": "dlcvcam-bootimg-manifest-v1",
        "file": img.name,
        "size": img.stat().st_size,
        "sha256": file_sha,
        "fit_images": [
            {
                "name": d.get("name"),
                "type": d.get("type"),
                "data_size": d.get("data_size"),
                "data_position": d.get("data_position"),
                "hashes": [
                    {
                        "node": h.get("node"),
                        "algo": h.get("algo"),
                        "sha": h.get("expected") or h.get("actual"),
                    }
                    for h in d.get("hashes", [])
                ],
            }
            for d in fit_details
        ],
        "meta": meta,
    }
    out.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return out


def cmd_verify(args: argparse.Namespace) -> int:
    img = Path(args.image)
    if not img.is_file():
        eprint(f"文件不存在: {img}")
        return 2

    size = img.stat().st_size
    if size <= 0:
        eprint("[FAIL] 空文件")
        return 1

    data = img.read_bytes()
    file_sha = hashlib.sha256(data).hexdigest()
    print(f"file      : {img}")
    print(f"size      : {size}")
    print(f"sha256    : {file_sha}")

    failed = False

    # whole-file hash
    try:
        expect = load_expect_sha256(img, args.expect_sha256, Path(args.sha256_file) if args.sha256_file else None)
    except VerifyError as ex:
        eprint(f"[ERR] {ex}")
        return 2

    if expect is None:
        if args.require_sidecar:
            eprint("[FAIL] 未提供/未找到整包 .sha256（指定了 --require-sidecar）")
            return 1
        print("whole-file: (no sidecar / --expect-sha256, skip)")
    else:
        if hmac_compare(bytes.fromhex(expect), bytes.fromhex(file_sha)):
            print(f"[OK]   whole-file sha256 matches")
        else:
            failed = True
            print(f"[FAIL] whole-file sha256 mismatch")
            print(f"       expect {expect}")
            print(f"       actual {file_sha}")

    # FIT internal hashes
    try:
        ok, messages, details = verify_fit_hashes(data)
    except VerifyError as ex:
        eprint(f"[FAIL] FIT 解析/校验错误: {ex}")
        return 1

    print("fit-hash  :")
    for m in messages:
        print(f"  {m}")
    if not ok:
        failed = True

    if args.json_out:
        Path(args.json_out).write_text(
            json.dumps(
                {
                    "file": str(img),
                    "size": size,
                    "sha256": file_sha,
                    "whole_file_ok": (expect is None) or (expect == file_sha),
                    "fit_ok": ok,
                    "fit_images": details,
                },
                indent=2,
                ensure_ascii=False,
            )
            + "\n",
            encoding="utf-8",
        )

    if failed:
        print("RESULT    : FAIL")
        return 1
    print("RESULT    : OK")
    return 0


def cmd_gen_sidecar(args: argparse.Namespace) -> int:
    img = Path(args.image)
    if not img.is_file():
        eprint(f"文件不存在: {img}")
        return 2
    data = img.read_bytes()
    file_sha = hashlib.sha256(data).hexdigest()

    sha_path = Path(args.sha256_file) if args.sha256_file else Path(str(img) + ".sha256")
    sha_path.write_text(f"{file_sha}  {img.name}\n", encoding="utf-8")

    fit_details: List[Dict[str, Any]] = []
    try:
        _ok, _msgs, fit_details = verify_fit_hashes(data)
    except VerifyError as ex:
        eprint(f"警告: FIT 解析失败，仍写入整包 sha256: {ex}")

    meta = {
        "build_version": args.build_version or "",
        "git": args.git or "",
        "kernelrelease": args.kernelrelease or "",
        "generated_by": "scripts/dlcvcam_verify_bootimg.py",
    }
    man_path = write_manifest(img, file_sha, fit_details, meta)

    print(f"wrote {sha_path}")
    print(f"wrote {man_path}")
    print(f"sha256 {file_sha}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="DLCVCAM boot.img hash verifier (no signature)")
    sub = p.add_subparsers(dest="cmd", required=True)

    v = sub.add_parser("verify", help="校验 boot.img（整包 sha256 + FIT 内嵌 hash）")
    v.add_argument("image", help="boot.img / boot-rk3576-*.img 路径")
    v.add_argument("--expect-sha256", help="期望的整包 sha256 hex")
    v.add_argument("--sha256-file", help="整包 sha256 sidecar 路径")
    v.add_argument("--require-sidecar", action="store_true", help="必须存在整包 sha256")
    v.add_argument("--json-out", help="把详细结果写到 JSON")
    v.set_defaults(func=cmd_verify)

    g = sub.add_parser("gen-sidecar", help="为 boot.img 生成 .sha256 与 .dlcvcam.json")
    g.add_argument("image", help="boot.img 路径")
    g.add_argument("--sha256-file", help="输出 .sha256 路径")
    g.add_argument("--build-version", default="")
    g.add_argument("--git", default="")
    g.add_argument("--kernelrelease", default="")
    g.set_defaults(func=cmd_gen_sidecar)

    return p


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.func(args))
    except BrokenPipeError:
        return 0
    except VerifyError as ex:
        eprint(f"[ERR] {ex}")
        return 2
    except OSError as ex:
        eprint(f"[ERR] {ex}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
