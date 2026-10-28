# -*- coding: utf-8 -*-
"""检查 GB2312 符号区里的"块状字符"在字库中的实际填充率。
如果填充率接近 100%，说明屏幕上的黑块就是这些字符本身，不是缺字。"""

BASE = r'D:\cc项目\触摸屏\server'
BIN = BASE + r'\gb2312_20x20.bin'
SIZE, BPC = 20, 60
ROW_BYTES = (SIZE + 7) // 8   # 3
HIGH0, LOW0, POS = 0xA1, 0xA1, 94

data = open(BIN, 'rb').read()


def glyph(h, l):
    zi, pi = h - HIGH0, l - LOW0
    off = (zi * POS + pi) * BPC
    return data[off:off + BPC]


def ink_ratio(g):
    """统计被点亮的像素比例"""
    on = 0
    total = SIZE * SIZE
    for row in range(SIZE):
        for col in range(SIZE):
            if g[row * ROW_BYTES + col // 8] & (0x80 >> (col % 8)):
                on += 1
    return on / total


# GB2312 符号区里常见的块状/装饰字符
cands = []
for h in range(0xA1, 0xAA):
    for l in range(0xA1, 0xFE + 1):
        try:
            ch = bytes((h, l)).decode('gb2312')
        except UnicodeDecodeError:
            continue
        g = glyph(h, l)
        if g == bytes(BPC):
            continue
        r = ink_ratio(g)
        cands.append((r, h, l, ch))

cands.sort(reverse=True)
print('=== 字库里"填充率最高"的 25 个字符（20x20）===')
print('  填充率   区位       字符')
for r, h, l, ch in cands[:25]:
    print('  %6.1f%%  0x%02X 0x%02X  %s' % (r * 100, h, l, ch))

print()
print('=== 常见分隔符的实际填充率 ===')
for name, ch in [('实心方块 U+25A0', '\u25a0'),
                 ('大方块 U+25A0', '■'),
                 ('菱形 ◆', '◆'),
                 ('实心圆 ●', '●'),
                 ('星 ★', '★'),
                 ('空 ★', '☆'),
                 ('双线 ═', '═'),
                 ('方块 █', '█'),
                 ('竖线 ｜', '｜'),
                 ('间隔号 ·', '·')]:
    try:
        b = ch.encode('gb2312')
    except UnicodeEncodeError:
        print('  %-16s %s  -> 不在 GB2312 中' % (name, ch))
        continue
    g = glyph(b[0], b[1])
    r = ink_ratio(g)
    blank = '  (全空!)' if g == bytes(BPC) else ''
    print('  %-16s %s  0x%02X%02X  填充率 %.1f%%%s' % (
        name, ch, b[0], b[1], r * 100, blank))
