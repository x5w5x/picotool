# -*- coding: utf-8 -*-
"""扫描字库里的"全 1"(实心方块) 和 "全 0"(空白) 字形，定位黑块来源。"""
import os

BASE = r'D:\cc项目\触摸屏\server'
FILES = [
    ('gb2312_16x16.bin', 16, 32),
    ('gb2312_20x20.bin', 20, 60),
    ('gb2312_24x24.bin', 24, 72),
]
HIGH0, LOW0 = 0xA1, 0xA1
ZONES, POS = 87, 94


def decodable(h, l):
    try:
        bytes((h, l)).decode('gb2312')
        return True
    except UnicodeDecodeError:
        return False


for fname, size, bpc in FILES:
    path = os.path.join(BASE, fname)
    data = open(path, 'rb').read()
    print('=' * 60)
    print('%s (%d 字/字)' % (fname, bpc))
    print('=' * 60)

    all_zero = 0
    all_ones = 0
    ones_list = []
    partial = 0

    for zi in range(ZONES):
        h = HIGH0 + zi
        for pi in range(POS):
            l = LOW0 + pi
            g = data[(zi * POS + pi) * bpc:(zi * POS + pi + 1) * bpc]
            if g == bytes(bpc):
                all_zero += 1
            elif g == b'\xff' * bpc:
                all_ones += 1
                if decodable(h, l):
                    ones_list.append((h, l))
            else:
                partial += 1

    print('  正常字形 : %d' % partial)
    print('  全 0(空白): %d' % all_zero)
    print('  全 1(实心方块): %d' % all_ones)
    if ones_list:
        print('  其中可解码的实心方块 %d 个:' % len(ones_list))
        for i, (h, l) in enumerate(ones_list[:25]):
            print('    0x%02X 0x%02X  %s' % (h, l, bytes((h, l)).decode('gb2312')))
    print()
