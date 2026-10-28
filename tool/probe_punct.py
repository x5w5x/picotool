# -*- coding: utf-8 -*-
"""检查小说里实际用到的标点，在字库中是否有正常字形。"""
import collections

BASE = r'D:\cc项目\触摸屏\server'
BIN = BASE + r'\gb2312_20x20.bin'
SIZE, BPC = 20, 60
ROW_BYTES = (SIZE + 7) // 8
HIGH0, LOW0, POS = 0xA1, 0xA1, 94

data = open(BIN, 'rb').read()


def glyph(h, l):
    off = ((h - HIGH0) * POS + (l - LOW0)) * BPC
    return data[off:off + BPC]


def ink(g):
    on = 0
    for row in range(SIZE):
        for col in range(SIZE):
            if g[row * ROW_BYTES + col // 8] & (0x80 >> (col % 8)):
                on += 1
    return on


def art(g):
    out = []
    for row in range(SIZE):
        line = ''
        for col in range(SIZE):
            bit = g[row * ROW_BYTES + col // 8] & (0x80 >> (col % 8))
            line += '#' if bit else '.'
        out.append(line)
    return '\n'.join(out)


# 1) 统计整个小说里出现的所有非 ASCII 字符
raw = open(r'C:\Users\xwx\Downloads\世子凶猛，姑娘请自重_gbk.txt', 'rb').read()
text = raw.decode('gbk', errors='replace')
cnt = collections.Counter(ch for ch in text if ord(ch) > 0x7F)
print('全文字符种类: %d' % len(cnt))
print()

# 2) 检查这些字符里哪些在字库里是空的
missing = []
for ch, n in cnt.items():
    try:
        b = ch.encode('gb2312')
    except UnicodeEncodeError:
        missing.append((ch, n, None))
        continue
    if len(b) != 2:
        continue
    g = glyph(b[0], b[1])
    if g == bytes(BPC):
        missing.append((ch, n, b))

print('=== 字库里字形为空(显示空白)的字符，按出现次数排序 ===')
if not missing:
    print('  无')
else:
    for ch, n, b in sorted(missing, key=lambda x: -x[1])[:30]:
        loc = '0x%02X%02X' % (b[0], b[1]) if b else '<不在GB2312>'
        print('  %r  出现 %5d 次   %s' % (ch, n, loc))

print()
print('=== 关键标点的字模检查 ===')
for ch in ['。', '、', '，', '【', '】', '？', '！', '：', '；', '“', '”', '《', '》', '·', '—', '…']:
    try:
        b = ch.encode('gb2312')
    except UnicodeEncodeError:
        print('  %r  不在 GB2312' % ch)
        continue
    g = glyph(b[0], b[1])
    ink_n = ink(g)
    tag = ''
    if g == bytes(BPC):
        tag = '  <<< 全空!(显示为空白)'
    print('  %r  0x%02X%02X  点亮 %3d/%d 像素%s' % (
        ch, b[0], b[1], ink_n, SIZE * SIZE, tag))

print()
print('=== 打印 【 】 。 、 的实际字模 ===')
for ch in ['【', '】', '。', '、']:
    b = ch.encode('gb2312')
    g = glyph(b[0], b[1])
    print('--- %r (0x%02X 0x%02X) 点亮 %d ---' % (ch, b[0], b[1], ink(g)))
    print(art(g))
    print()
