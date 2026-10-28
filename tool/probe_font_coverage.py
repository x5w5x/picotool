# -*- coding: utf-8 -*-
"""检查字库 bin 的完整性：统计空白字形，定位空缺区位。"""
import os

BASE = r'D:\cc项目\触摸屏\server'
FILES = [
    ('gb2312_16x16.bin', 16, 32),
    ('gb2312_20x20.bin', 20, 60),
    ('gb2312_24x24.bin', 24, 72),
]
HIGH0, LOW0 = 0xA1, 0xA1
ZONES = 87
POS = 94


def gb2312_decodable(high, low):
    try:
        bytes((high, low)).decode('gb2312')
        return True
    except UnicodeDecodeError:
        return False


for fname, size, bpc in FILES:
    path = os.path.join(BASE, fname)
    if not os.path.exists(path):
        print('%s: 缺失' % fname)
        continue
    data = open(path, 'rb').read()
    total = ZONES * POS
    print('=' * 62)
    print('%s  大小 %d 字节  期望 %d  字/字 %d' % (
        fname, len(data), total * bpc, bpc))
    print('=' * 62)

    blank = 0
    decodable_but_blank = []
    decodable_count = 0
    blank_by_zone = {}

    for zi in range(ZONES):
        high = HIGH0 + zi
        zone_blank = 0
        zone_total = 0
        for pi in range(POS):
            low = LOW0 + pi
            off = (zi * POS + pi) * bpc
            glyph = data[off:off + bpc]
            is_blank = (glyph == bytes(bpc))
            ok = gb2312_decodable(high, low)
            if ok:
                decodable_count += 1
            zone_total += 1
            if is_blank:
                blank += 1
                zone_blank += 1
                if ok:
                    decodable_but_blank.append((high, low))
        if zone_blank == zone_total and zone_total > 0:
            blank_by_zone[high] = zone_blank

    print('  可解码 GB2312 字符数: %d' % decodable_count)
    print('  全零(空白)字形      : %d / %d' % (blank, total))
    print('  **可解码但字模为空** : %d   <-- 这些就是屏幕上的空白' % len(decodable_but_blank))

    # 按区统计
    print()
    print('  各区位空缺情况(仅列出空白数 > 0 的区):')
    for zi in range(ZONES):
        high = HIGH0 + zi
        cnt = 0
        for pi in range(POS):
            off = (zi * POS + pi) * bpc
            if data[off:off + bpc] == bytes(bpc):
                cnt += 1
        if cnt:
            kind = ''
            if 0xB0 <= high <= 0xF7:
                kind = '(汉字区)'
            elif 0xA1 <= high <= 0xA9:
                kind = '(符号区)'
            print('    区 0x%02X%s: 空白 %d/94' % (high, kind, cnt))

    if decodable_but_blank:
        print()
        print('  前 20 个"可解码但空字模"的字符:')
        for (h, l) in decodable_but_blank[:20]:
            ch = bytes((h, l)).decode('gb2312')
            print('    0x%02X 0x%02X  %s' % (h, l, ch))
    print()
