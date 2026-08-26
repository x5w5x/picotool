import struct, os
files = [
    'STM32F10x_16.FLM', 'STM32F10x_128.FLM', 'STM32F10x_512.FLM',
    'STM32F10x_1024.FLM', 'STM32F10x_CL.FLM', 'STM32F10x_OPT.FLM'
]
base = r'C:\Keil_v5\ARM\PACK\Keil\STM32F1xx_DFP\2.4.1\Flash'
for f in files:
    p = os.path.join(base, f)
    with open(p, 'rb') as fh:
        d = fh.read(64)
    if d[:4] == b'\x7fELF':
        cls = 'ELF %d' % d[4]
    else:
        cls = 'magic=' + d[:4].hex()
    print(f'{f:24s} {cls:10s}  first64={d[:16].hex()}')