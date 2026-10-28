# -*- coding: utf-8 -*-
"""用真实字库渲染小说首页，模拟设备显示，输出 PNG 便于和实物对比。"""
from PIL import Image

BASE = r'D:\cc项目\触摸屏\server'
BIN = BASE + r'\gb2312_20x20.bin'
NOVEL = r'C:\Users\xwx\Downloads\世子凶猛，姑娘请自重_gbk.txt'
OUT = r'E:\pico\pico\tool\mock_page.png'

SIZE, BPC = 20, 60
ROW_BYTES = (SIZE + 7) // 8
HIGH0, LOW0, POS = 0xA1, 0xA1, 94

# 与 plugin_novel_reader.c 一致的布局
SCREEN_W, SCREEN_H = 480, 320
MARGIN_X = 10
HEADER_H = 32
BODY_Y = HEADER_H + 4
BODY_H = 240
char_w_ascii, char_w_cjk, line_h = 10, 20, 24
max_x = SCREEN_W - MARGIN_X * 2
max_page_lines = BODY_H // line_h

font = open(BIN, 'rb').read()


def glyph(h, l):
    off = ((h - HIGH0) * POS + (l - LOW0)) * BPC
    return font[off:off + BPC]


def blank():
    return bytes(BPC)


img = Image.new('RGB', (SCREEN_W, SCREEN_H), (255, 255, 255))
px = img.load()


def draw_cjk(cx, cy, g):
    for row in range(SIZE):
        for col in range(SIZE):
            if g[row * ROW_BYTES + col // 8] & (0x80 >> (col % 8)):
                x, y = cx + col, cy + row
                if 0 <= x < SCREEN_W and 0 <= y < SCREEN_H:
                    px[x, y] = (0, 0, 0)


# 简单 ASCII 5x7（只画个占位方块，够对比布局）
def draw_ascii(cx, cy, ch):
    for row in range(8):
        for col in range(5):
            if (ord(ch) * 7 + row * 3 + col) % 3 == 0:
                x, y = cx + col, cy + row
                if 0 <= x < SCREEN_W and 0 <= y < SCREEN_H:
                    px[x, y] = (0, 0, 0)


text = open(NOVEL, 'rb').read().decode('gbk', errors='replace')
# 从"简介："开始
start = text.find('简介：')
if start < 0:
    start = 0
body = text[start:]

# 分行（复刻修复后的算法）
lines = []
cur = []
cur_x = 0
for ch in body:
    try:
        b = ch.encode('gb2312')
    except UnicodeEncodeError:
        b = None
    if ch == '\n':
        lines.append(''.join(cur))
        cur, cur_x = [], 0
        if len(lines) > max_page_lines:
            break
        continue
    w = char_w_ascii if ch.isascii() else char_w_cjk
    if cur_x + w > max_x and cur_x > 0:
        lines.append(''.join(cur))
        cur, cur_x = [], 0
        if len(lines) > max_page_lines:
            break
    cur.append(ch)
    cur_x += w
    if len(lines) > max_page_lines:
        break

# 渲染
y = BODY_Y
drawn = 0
for ln in lines[:max_page_lines]:
    x = MARGIN_X
    for ch in ln:
        if x >= SCREEN_W - MARGIN_X:
            break
        if ch.isascii():
            draw_ascii(x, y + 6, ch)
            x += char_w_ascii
        else:
            try:
                b = ch.encode('gb2312')
                g = glyph(b[0], b[1])
            except UnicodeEncodeError:
                g = blank()      # 缺字 = 不画（保持白底）
            if g != blank():
                draw_cjk(x, y, g)
            x += char_w_cjk
        drawn += 1
    y += line_h
    if y >= BODY_Y + BODY_H:
        break

img.save(OUT)
print('已生成:', OUT)
print('渲染字符数:', drawn)
print()
print('前几行内容:')
for i, ln in enumerate(lines[:8]):
    print('  %d: %s' % (i, ln))
