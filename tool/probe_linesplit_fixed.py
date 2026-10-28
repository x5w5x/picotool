# -*- coding: utf-8 -*-
"""验证修复后的分行算法：每行字节数必须是偶数且无错位。"""

MARGIN_X = 10
SCREEN_W = 480
BODY_H = 240
char_w_ascii = 10
char_w_cjk = 20
line_h = 24
max_x = SCREEN_W - MARGIN_X * 2
max_page_lines = BODY_H // line_h


def split_lines_fixed(gbk_bytes):
    """复刻修复后的逻辑：[start, end) 偏移，不破坏缓冲区"""
    buf = bytearray(gbk_bytes)
    gbk_len = len(buf)
    starts = [0]
    ends = []
    byte_i = 0
    cur_x = 0
    lc = 0
    while byte_i < gbk_len and lc <= max_page_lines:
        b = buf[byte_i]
        if b == 0x0A:  # \n
            ends.append(byte_i)
            byte_i += 1
            lc += 1
            starts.append(byte_i)
            cur_x = 0
            continue
        if b == 0x0D:  # \r
            ends.append(byte_i)
            byte_i += 1
            if byte_i < gbk_len and buf[byte_i] == 0x0A:
                byte_i += 1
            lc += 1
            starts.append(byte_i)
            cur_x = 0
            continue
        if b < 0x80:
            ch_w, ch_bytes = char_w_ascii, 1
        else:
            ch_w, ch_bytes = char_w_cjk, 2
            if byte_i + 1 >= gbk_len:
                break
        if cur_x + ch_w > max_x and cur_x > 0:
            ends.append(byte_i)          # 本行结束
            lc += 1
            starts.append(byte_i)        # 下一行从同一位置开始（不丢字节）
            cur_x = 0
            continue
        cur_x += ch_w
        byte_i += ch_bytes

    count = min(lc, max_page_lines)
    lines = []
    for i in range(count):
        st, en = starts[i], ends[i]
        if en > st:
            lines.append(bytes(buf[st:en]))
    return lines


def check(name, text, crlf=False):
    data = text.encode('gbk')
    lines = split_lines_fixed(data)
    print('--- %s (原文 %d 字节) ---' % (name, len(data)))
    all_ok = True
    for i, ln in enumerate(lines):
        even = (len(ln) % 2 == 0)
        if not even:
            all_ok = False
        try:
            txt = ln.decode('gbk')
            dec_ok = True
        except Exception:
            txt = '<解码失败>'
            dec_ok = False
            all_ok = False
        print('  行%d len=%2d %s %-30s' % (
            i, len(ln), '偶' if even else '奇!!', txt[:28]))
    print('  =>', 'PASS' if all_ok else '**FAIL**')
    print()


para1 = ('高祖授其开国辅运推诚宣力武臣特进光禄大夫右柱国太傅参军国事'
         '改封徐国公岁禄五千石世袭丹书铁券')
para2 = '如燕王世子那般优秀不仅武艺无双听说连长'
para3 = '楼际撺媪摅嗅嘤退忝块准突惔强吹敔不兜纳'

check('纯自动换行（长段落）', para1)
check('多段自动换行', para1 + para2 + para3)
check('LF 换行', para1 + '\n' + para2 + '\n' + para3)
check('CRLF 换行', para1 + '\r\n' + para2 + '\r\n' + para3)
