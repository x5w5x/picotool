# -*- coding: utf-8 -*-
"""穷举字节偏移，统计错位后能拼出哪些"密集/方块类"字符。"""
import collections

DENSE = set('■□▲△●○◆◇★☆▓▒░█▇▆▅▄▃▂▁※〓⊙◎')
NORMAL = set('·、。，；：！？（）《》【】')

# 用一段真实小说文本做底料
TEXT = ('书名：世子凶猛，高祖授其开国辅运推诚宣力武臣特进光禄大夫右柱国太傅参军国事，'
        '改封徐国公，岁禄五千石，世袭丹书铁券。如燕王世子那般优秀，不仅武艺无双，'
        '听说连长楼际撺媪摅嗅嘤退忝块准突惔强吹敔不兜纳靡链枪兰兰容换断康藏换肩蛰犷')

raw = TEXT.encode('gbk')
print('底料 %d 字节' % len(raw))
print()

stats = collections.Counter()
found = collections.Counter()

for off in range(0, 8):
    mis = raw[off:]
    i = 0
    while i < len(mis) - 1:
        pair = mis[i:i + 2]
        try:
            ch = pair.decode('gbk')
        except UnicodeDecodeError:
            stats['无效'] += 1
            i += 2
            continue
        try:
            ch.encode('gb2312')
            stats['GB2312合法'] += 1
        except UnicodeEncodeError:
            stats['GBK扩展/超出'] += 1
        if ch in DENSE:
            found[ch] += 1
        i += 2

print('错位解码结果统计（偏移 0..7 合计）:')
for k, v in stats.items():
    print('   %-12s %d' % (k, v))
print()
print('其中拼出的"密集/方块类"字符:')
if found:
    for ch, n in found.most_common():
        print('   %r  出现 %d 次   GBK=%s' % (ch, n, ch.encode('gbk').hex(' ')))
else:
    print('   无')

print()
print('=' * 60)
print('结论')
print('=' * 60)
print('字节错位会产生大量"合法的 GB2312 字符"——这就是乱码字的来源。')
if found:
    print('并且确实能拼出密集符号（如 %s），它们在屏幕上呈现为')
    print('实心黑方块。**因此黑色方块不需要用"字库缺失"来解释。**')
    print(' '.join(repr(c) for c in list(found)[:10]))
else:
    print('但这个样本里没有拼出密集符号，说明黑块可能另有来源。')
