#!/usr/bin/env python3
"""将小说 TXT 预处理为阅读器使用的 GBK 文本。

用法：
  python tools/novel_txt_to_gbk.py input.txt
  python tools/novel_txt_to_gbk.py input.txt output_gbk.txt

输入优先按 UTF-8（含/不含 BOM）读取，失败时尝试 GB18030；输出为
GBK/CP936，并统一为 LF 换行，避免阅读器运行时做 UTF-8 分页转换。
"""

from __future__ import annotations

import argparse
from pathlib import Path


def read_text(path: Path) -> tuple[str, str]:
    raw = path.read_bytes()
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw[3:].decode("utf-8"), "UTF-8 BOM"
    try:
        return raw.decode("utf-8"), "UTF-8"
    except UnicodeDecodeError:
        return raw.decode("gb18030"), "GB18030/GBK"


def main() -> int:
    parser = argparse.ArgumentParser(description="Convert novel TXT to GBK/CP936")
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path, nargs="?")
    args = parser.parse_args()

    text, source_encoding = read_text(args.input)
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    output = args.output or args.input.with_name(args.input.stem + "_gbk.txt")

    try:
        encoded = text.encode("gbk")
        replaced = 0
    except UnicodeEncodeError:
        encoded = text.encode("gbk", errors="replace")
        replaced = text.encode("gbk", errors="replace").count(b"?")

    output.write_bytes(encoded)
    print(f"输入: {args.input}")
    print(f"识别编码: {source_encoding}")
    print(f"输出: {output}")
    print(f"大小: {len(encoded)} 字节")
    if replaced:
        print(f"警告: {replaced} 个无法转换的字符被替换为 ?")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
