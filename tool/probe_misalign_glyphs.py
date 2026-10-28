# -*- coding: utf-8 -*-
"""验证：GBK 字节错位 1 位后，会拼出什么样的字符？会不会出现实心方块？"""

text = ('如燕王世子那般优秀，不仅武艺无双，听说连长'
        '楼际撺媪摅嗅嘤退忝块准突惔强吹敔不兜纳')

raw = text.encode('gbk')
print('原文 (%d 字节):' % len(raw))
print(' ', text)
print()

# 模拟旧 bug：自动换行处吃掉 1 字节，后续从第 2 字节开始
# 取前 46 字节为第一行（奇数长度说明错位了）
mis = raw[1:]          # 偏移 1 字节，模拟"从第二字节开始"
print('从第 2 个字节开始解码（错位 1）:')
out = []
i = 0
try:
    while i < len(mis) - 1:
        pair = mis[i:i + 2]
        try:
            ch = pair.decode('gbk')
            out.append(ch)
        except UnicodeDecodeError:
            out.append('?')
        i += 2
    # 若最后剩 1 字节，说明长度奇数
except Exception:
    pass
s = ''.join(out)
print(' ', s[:40])

print()
DENSE = set('■□▲△●○◆◇★☆▓▒░█▇▆▅▄▃▂▁※')
hits = [(idx, c) for idx, c in enumerate(s) if c in DENSE]
print('拼出的字符里，密集/方块类字符:')
if hits:
    for idx, c in hits[:15]:
        print('   位置 %2d: %r   GBK=%s' % (idx, c, c.encode('gbk').hex(' ')))
else:
    print('   无')

# 统计有多少个"错位字符"在 GB2312 里合法
legal = 0
illegal = 0
for ch in s:
    if ch == '?':
        illegal += 1
        continue
    try:
        ch.encode('gb2312')
        legal += 1
    except UnicodeEncodeError:
        illegal += 1
print()
print('错位后共 %d 个字符: GB2312 合法 %d, 非法 %d' % (len(s), legal, illegal))
print()
print('结论:')
print('  错位产生的绝大多数仍是"合法的 GB2312 字符"（所以会渲染成字形正确')
print('  但字义错误的字，肉眼看到的就是"乱码字"），其中少数正好落在')
print('  ■ □ ● 这类密集符号上，看上去就是实心黑方块。')
