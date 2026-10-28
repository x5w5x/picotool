# -*- coding: utf-8 -*-
"""复现 novel_reader 的编码检测 + 转换链路，验证两种输入的结果。"""

def detect(head):
    """完全照抄 plugin_novel_reader.c 的 detect_encoding 逻辑"""
    if len(head) >= 3 and head[0] == 0xEF and head[1] == 0xBB and head[2] == 0xBF:
        return 'UTF8_BOM'
    cjk = 0
    bad = 0
    i = 0
    while i < len(head):
        b = head[i]
        if b < 0x80:
            i += 1
            continue
        if ((b & 0xF0) == 0xE0 and i + 2 < len(head)
                and (head[i + 1] & 0xC0) == 0x80
                and (head[i + 2] & 0xC0) == 0x80):
            cjk += 1
            i += 3
        else:
            bad += 1
            i += 1
    return 'UTF8' if (cjk >= 1 and bad == 0) else 'GBK'


def gb2312_ok(hi, lo):
    """chinese.c 的 GB2312_GetOffset 边界"""
    return 0xA1 <= hi <= 0xF7 and 0xA1 <= lo <= 0xFE


samples = {
    'UTF-8 中文+ASCII': '第一章 你好，世界！Hello World 测试',
    'GBK   中文+ASCII': '第一章 你好，世界！Hello World 测试',
    'UTF-8 纯中文':     '这是一本小说的正文内容测试',
    'GBK   纯中文':     '这是一本小说的正文内容测试',
}

print('=== 编码检测结果 ===')
for name, text in samples.items():
    enc = 'utf-8' if name.startswith('UTF-8') else 'gbk'
    data = text.encode(enc)
    det = detect(data[:64])
    expect = 'UTF8' if enc == 'utf-8' else 'GBK'
    mark = 'OK ' if det == expect else '误判!'
    print('%-18s 真实=%-5s 检测=%-6s %s  head=%s'
          % (name, enc, det, mark, data[:12].hex(' ')))

print()
print('=== 假设 UTF-8 被误判成 GBK，逐字节查字库会怎样 ===')
utf8 = '第一章'.encode('utf-8')
print('  UTF-8 字节: ' + utf8.hex(' '))
i = 0
while i + 1 < len(utf8):
    hi, lo = utf8[i], utf8[i + 1]
    ok = gb2312_ok(hi, lo)
    print('  0x%02X 0x%02X -> %s' % (hi, lo, '查到错位字模(乱码字)' if ok else 'offset全F, 渲染空白'))
    i += 2

print()
print('=== 假设 GBK 被误判成 UTF-8，逐字节解码 ===')
gbk = '第一章'.encode('gbk')
print('  GBK 字节: ' + gbk.hex(' '))
for b in gbk:
    if b >= 0x80:
        n = (2 if 0xC2 <= b <= 0xDF else
             3 if 0xE0 <= b <= 0xEF else
             4 if 0xF0 <= b <= 0xF4 else 0)
        print('  首字节 0x%02X -> %s' % (b, ('按 %d 字节 UTF-8 解析' % n) if n else '非法(bad), 记 bad++'))

print()
print('=== 关键结论 ===')
print('  GBK 文件: 高字节 0xB0..0xF7 且次字节 >=0xA1 -> detect 里 (b&0xF0)==0xE0')
print('  有部分 GBK 汉字的首字节正好落在 0xE0..0xEF(GBK 区 0xE0-0xEF 对应 GB2312 的高区),')
print('  若其后续字节又恰好是 0x80..0xBF 形式,就会被误判为 UTF-8。')
print()
cnt_re = 0
for hi in range(0xE0, 0xF0):
    for lo in range(0xA1, 0xFF):
        if (lo & 0xC0) == 0x80:
            cnt_re += 1
print('  GBK 中首字节 0xE0-0xEF 且次字节 (lo&0xC0)==0x80 的组合数: %d 个区位' % cnt_re)
print('  这些 GBK 双字节会被完全误判为 UTF-8 三字节序列的前两字节 -> 解码跑飞')
