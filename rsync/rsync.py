#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
基于 rsync 思想的固定块大小“滑动窗口 + 滚动校验和”比较两个文件的冗余率。
- 弱校验和：rsync rolling checksum（s1/s2）
- 强校验和：MD5
- 预处理目标文件（basis）按固定 block_size 分块，计算每块 (weak, md5)
- 在源文件（new）上按字节滑动 rolling checksum，命中 weak 后再比对 md5，命中则认定一个 block_size 的匹配并跳过 block_size；否则继续滑动 1 字节
- 统计：
  Matched bytes = 所有命中的块大小总和（block_size 的倍数）
  Literal bytes = 源文件总大小 - Matched bytes（也可按逐字节累计）
  冗余率 = Matched / (Matched + Literal)

使用：
  1) 自动试多种块大小（默认：512, 1024, 2048, 4096, 8192）：
     python3 rsync_window_compare.py [源文件 new] [目标文件 basis]
     例如：python3 rsync_window_compare.py random.txt random_copy.txt

  2) 指定若干块大小（空格分隔多个）：
     python3 rsync_window_compare.py [new] [basis] 1024 2048 4096

默认：
  new = 200KB1.txt
  basis = 200KB.txt
  sizes = [512, 1024, 2048, 4096, 8192]
"""

import sys
import os
import hashlib
from typing import Dict, List, Tuple

DEFAULT_NEW = "50M1.txt"
DEFAULT_BASIS = "50M.txt"
DEFAULT_SIZES = [512, 1024, 2048, 4096, 8192]

# ---------- rsync rolling checksum 实现 ----------
class RollingChecksum:
    def __init__(self, block_size: int):
        self.block_size = block_size
        self.s1 = 0
        self.s2 = 0

    def reset(self, block: bytes):
        # s1 = sum(block[i])
        # s2 = b0*n + b1*(n-1) + ... + b(n-1)*1
        b = block
        self.s1 = sum(b)
        n = self.block_size
        s2 = 0
        for i, x in enumerate(b):
            s2 += (n - i) * x
        self.s2 = s2

    def roll(self, out_byte: int, in_byte: int):
        # s1' = s1 - out + in
        # s2' = s2 - block_size*out + s1'
        self.s1 = self.s1 - out_byte + in_byte
        self.s2 = self.s2 - self.block_size * out_byte + self.s1

    def weak(self) -> Tuple[int, int]:
        return (self.s1 & 0xffffffff, self.s2 & 0xffffffff)

    @staticmethod
    def weak_tuple_to_key(s1: int, s2: int) -> int:
        # 合并为一个 64bit key 以便做 dict 查询
        return ((s2 & 0xffffffff) << 32) | (s1 & 0xffffffff)

# ---------- 辅助函数 ----------

def md5_hex(b: bytes) -> str:
    return hashlib.md5(b).hexdigest()

# ---------- 核心计算 ----------

def build_basis_index(basis_bytes: bytes, block_size: int):
    idx: Dict[int, List[Tuple[int, str]]] = {}
    n = len(basis_bytes)
    blocks = n // block_size
    if blocks <= 0:
        return idx, 0
    for bi in range(blocks):
        start = bi * block_size
        chunk = basis_bytes[start:start + block_size]
        rc = RollingChecksum(block_size)
        rc.reset(chunk)
        s1, s2 = rc.weak()
        key = RollingChecksum.weak_tuple_to_key(s1, s2)
        h = md5_hex(chunk)
        idx.setdefault(key, []).append((bi, h))
    return idx, blocks

def scan_new_against_basis(new_bytes: bytes, basis_idx, block_size: int):
    n = len(new_bytes)
    if n < block_size:
        return 0, n

    matched = 0
    literal = 0

    pos = 0
    rc = RollingChecksum(block_size)
    rc.reset(new_bytes[pos:pos + block_size])

    while pos + block_size <= n:
        s1, s2 = rc.weak()
        key = RollingChecksum.weak_tuple_to_key(s1, s2)
        candidates = basis_idx.get(key)
        hit = False
        if candidates:
            strong = md5_hex(new_bytes[pos:pos + block_size])
            for _, h in candidates:
                if h == strong:
                    matched += block_size
                    pos += block_size
                    if pos + block_size <= n:
                        rc.reset(new_bytes[pos:pos + block_size])
                    hit = True
                    break
        if hit:
            continue
        # 未命中，滑动 1 字节
        literal += 1
        out_b = new_bytes[pos]
        pos += 1
        if pos + block_size <= n:
            in_b = new_bytes[pos + block_size - 1]
            rc.roll(out_b, in_b)
        else:
            break

    # 结尾不足一个块的字节算 literal
    tail = n - pos
    if tail > 0:
        literal += tail

    return matched, literal


def compute_for_size(new_bytes: bytes, basis_bytes: bytes, block_size: int):
    basis_idx, _ = build_basis_index(basis_bytes, block_size)
    matched, literal = scan_new_against_basis(new_bytes, basis_idx, block_size)
    total = matched + literal
    redundancy = (matched * 100.0 / total) if total > 0 else 0.0
    return {
        'block_size': block_size,
        'matched': matched,
        'literal': literal,
        'total': total,
        'redundancy': redundancy,
    }

# ---------- 主函数 ----------

def main():
    # 参数解析
    new_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_NEW
    basis_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_BASIS

    sizes: List[int] = []
    if len(sys.argv) > 3:
        # 第三个及之后参数作为块大小列表
        for s in sys.argv[3:]:
            try:
                sizes.append(int(s))
            except Exception:
                pass
    if not sizes:
        sizes = DEFAULT_SIZES[:]

    # 文件读取
    if not os.path.isfile(new_path):
        print(f"[错误] 源文件不存在: {new_path}", file=sys.stderr)
        sys.exit(1)
    if not os.path.isfile(basis_path):
        print(f"[警告] 目标文件不存在: {basis_path}（将视为完全不同）", file=sys.stderr)
        basis_bytes = b''
    else:
        with open(basis_path, 'rb') as f:
            basis_bytes = f.read()
    with open(new_path, 'rb') as f:
        new_bytes = f.read()

    # 逐尺寸计算
    results = []
    for bs in sizes:
        if bs <= 0:
            continue
        r = compute_for_size(new_bytes, basis_bytes, bs)
        results.append(r)

    # 选择最优（冗余率最高）
    best = max(results, key=lambda x: (x['redundancy'], -x['block_size'])) if results else None

    # 输出
    print("========== rsync-风格滑动窗口冗余率（多块大小） ==========")
    print(f"源文件: {new_path}")
    print(f"目标文件: {basis_path}")
    print(f"候选块大小: {', '.join(str(x) for x in sizes)}")
    print()
    print("逐块大小结果：")
    for r in results:
        print(f"  - 块: {r['block_size']:>5} | Matched: {r['matched']:>8} | Literal: {r['literal']:>8} | Total: {r['total']:>8} | 冗余率: {r['redundancy']:6.2f}%")
    print()
    if best:
        print("最佳选择：")
        print(f"  块大小: {best['block_size']} bytes")
        print(f"  冗余率: {best['redundancy']:.2f}% (Matched {best['matched']} / Total {best['total']})")
    print("======================================================")

if __name__ == '__main__':
    main()
