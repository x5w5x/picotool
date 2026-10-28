# -*- coding: utf-8 -*-
"""
原样复刻 plugin_novel_reader.c 里 render_current_page() 的分行 + 渲染逻辑，
验证"自动换行"和"显式换行"两种情况下，切出来的行是否对齐。
"""

PAGE_BUF_SIZE = 2400
MARGIN_X = 10
SCREEN_W = 480
BODY_Y = 44
BODY_H = 240
# 中字体
char_w_ascii = 10
char_w_cjk = 20
line_h = 24

max_x = SCREEN_W - MARGIN_X * 2
max_page_lines = BODY_H // line_h


def split_lines(gbk_bytes):
    """完全照抄 C 代码的分行逻辑"""
    buf = bytearray(gbk_bytes)
    gbk_len = len(buf)
    line_ends = []
    byte_i = 0
    cur_x = 0
    while byte_i < gbk_len and len(line_ends) <= max_page_lines:
        b = buf[byte_i]
        if b == 0x0A:  # \n
            line_ends.append(byte_i)
            byte_i += 1
            cur_x = 0
            continue
        if b == 0x0D:  # \r
            line_ends.append(byte_i)
            byte_i += 1
            if byte_i < gbk_len and buf[byte_i] == 0x0A:
                byte_i += 1
            cur_x = 0
            continue
        if b < 0x80:
            ch_w, ch_bytes = char_w_ascii, 1
        else:
            ch_w, ch_bytes = char_w_cjk, 2
            if byte_i + 1 >= gbk_len:
                break
        if cur_x + ch_w > max_x and cur_x > 0:
            line_ends.append(byte_i)   # ← 记录"当前字符起点"
            cur_x = 0
            continue                   # ← 不推进 byte_i
        cur_x += ch_w
        byte_i += ch_bytes

    # 在 line_ends 位置写 \0
    for pos in line_ends:
        if pos < PAGE_BUF_SIZE:
            buf[pos] = 0

    # 渲染时按 \0 切分
    lines = []
    p = 0
    while p < gbk_len and buf[p] != 0:
        e = p
        while e < gbk_len and buf[e] != 0:
            e += 1
        lines.append(bytes(buf[p:e]))
        p = e + 1
    return lines


def check(lines, original):
    print('  切出 %d 行:' % len(lines))
    ok = 0
    for i, ln in enumerate(lines):
        try:
            txt = ln.decode('gbk')
        except Exception:
            txt = '<解码失败>'
        # 判断这行在原文中是否存在（对齐则一定能找到）
        aligned = ln in original
        if aligned:
            ok += 1
        flag = 'OK ' if aligned else '错位!'
        print('   %2d %s len=%2d  %s' % (i, flag, len(ln), txt[:28]))
    print('  对齐行数: %d / %d' % (ok, len(lines)))
    return ok, len(lines)


print('=' * 64)
print('实验 A:纯自动换行（长段落无换行符，模拟真实小说）')
print('=' * 64)
para = ('高祖授其开国辅运推诚宣力武臣特进光禄大夫右柱国太傅参军国事'
        '改封徐国公岁禄五千石世袭丹书铁券')
data = para.encode('gbk')
print('原文 (%d 字节):' % len(data))
print(' ', para)
print()
lines = split_lines(data)
check(lines, data)

print()
print('=' * 64)
print('实验 B:每行都带显式 \\n（Windows 风格 \\r\\n 也算）')
print('=' * 64)
lines_txt = ['如燕王世子那般优秀不仅武艺无双听说连长',
             '楼际撺媪摅嗅嘤退忝块准突惔强吹敔不兜纳',
             '靡链枪兰兰容换断康藏换肩蛰犷']
data2 = ('\r\n'.join(lines_txt)).encode('gbk')
print('原文 (%d 字节)' % len(data2))
lines2 = split_lines(data2)
check(lines2, data2)

print()
print('=' * 64)
print('实验 C:混合（第一段自动换行后接显式换行）')
print('=' * 64)
text = para + '\r\n' + '如燕王世子那般优秀不仅武艺无双听说连长'
data3 = text.encode('gbk')
lines3 = split_lines(data3)
check(lines3, data3)
