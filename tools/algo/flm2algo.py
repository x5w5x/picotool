#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
flm2algo.py — 把 Keil ARM FLM 文件转换成 Pico 离线烧录器使用的 .algo 格式。

输出布局 = [64 字节 algo_header_t][code_size 字节机器码]
code_size 包含 PrgCode + PrgData 二者拼接(Keil 加载时连续拷贝)。
DevDscr 段不打包到目标,但在主机端用于读取 flash 描述。

用法:
    python flm2algo.py STM32F10x_128.FLM
    python flm2algo.py STM32F10x_16.FLM  STM32F10x_512.FLM  ...
    python flm2algo.py --all
"""
import struct
import os
import sys
import argparse

ALGO_MAGIC = 0x4F474C41   # "ALGO"

# ELF32
ELFCLASS32   = 1
ELFDATA2LSB  = 1
EM_ARM       = 40
SHT_SYMTAB   = 2
SHT_STRTAB   = 3
SHT_PROGBITS  = 1
SHT_NOBITS   = 8

# Keil FlashDevice 结构偏移(全部小端)
FLASHDEV_OFF_VER      = 0
FLASHDEV_OFF_NAME     = 2
FLASHDEV_OFF_TYPE     = 130
FLASHDEV_OFF_ADR      = 132
FLASHDEV_OFF_SZDEV    = 136
FLASHDEV_OFF_SZPAGE   = 140
FLASHDEV_OFF_RES      = 144
FLASHDEV_OFF_VALEMPTY = 148
FLASHDEV_OFF_TOPROG   = 149
FLASHDEV_OFF_TOERASE  = 150
FLASHDEV_OFF_RESERVED = 151
FLASHDEV_OFF_PROGSIZE = 152
FLASHDEV_OFF_RAMADR   = 156
FLASHDEV_OFF_RAMSIZE  = 160
FLASHDEV_TOTAL        = 168   # 结构至少这么多

class Elf:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.raw = f.read()
        self.path = path
        self._parse()

    def _u16(self, off):  return struct.unpack_from('<H', self.raw, off)[0]
    def _u32(self, off):  return struct.unpack_from('<I', self.raw, off)[0]

    def _parse(self):
        raw = self.raw
        if raw[:4] != b'\x7fELF':
            raise ValueError('not ELF')
        if raw[4] != ELFCLASS32 or raw[5] != ELFDATA2LSB:
            raise ValueError('need ELF32 LSB')
        if self._u16(18) != EM_ARM:
            raise ValueError('need ARM ELF')
        self.e_shoff      = self._u32(32)
        self.e_shentsize  = self._u16(46)
        self.e_shnum      = self._u16(48)
        self.e_shstrndx   = self._u16(50)

        # section headers
        self.sh = []
        for i in range(self.e_shnum):
            o = self.e_shoff + i * self.e_shentsize
            self.sh.append({
                'name_off':  self._u32(o + 0),
                'type':      self._u32(o + 4),
                'flags':     self._u32(o + 8),
                'addr':      self._u32(o + 12),
                'offset':    self._u32(o + 16),
                'size':      self._u32(o + 20),
                'link':      self._u32(o + 24),
                'info':      self._u32(o + 28),
                'addralign': self._u32(o + 32),
                'entsize':   self._u32(o + 36),
            })

        # 段名字符串表
        self.shstrtab_off = self.sh[self.e_shstrndx]['offset']
        self.shstrtab_sz  = self.sh[self.e_shstrndx]['size']

        # 找各 section + 符号表
        self.sections = {}   # name -> sh dict
        self.symtab   = None
        self.symstrtab_off = None
        self.symstrtab_sz  = None
        for s in self.sh:
            name = self._get_sh_name(s['name_off'])
            self.sections[name] = s
            if s['type'] == SHT_SYMTAB:
                self.symtab = s
                link = s['link']
                self.symstrtab_off = self.sh[link]['offset']
                self.symstrtab_sz  = self.sh[link]['size']

    def _get_sh_name(self, off):
        end = self.raw.index(b'\x00', self.shstrtab_off + off)
        return self.raw[self.shstrtab_off + off:end].decode('ascii', errors='replace')

    def _get_sym_name(self, off):
        end = self.raw.index(b'\x00', self.symstrtab_off + off)
        return self.raw[self.symstrtab_off + off:end].decode('ascii', errors='replace')

    def get_section_bytes(self, name):
        s = self.sections.get(name)
        if not s:
            return None
        return self.raw[s['offset']: s['offset'] + s['size']]

    def get_symbol_value(self, name):
        """返回符号的 st_value(Thumb 地址,bit0=1),找不到返回 None"""
        if not self.symtab:
            return None
        off = self.symtab['offset']
        sz  = self.symtab['size']
        ent = self.symtab['entsize'] or 16
        for i in range(sz // ent):
            o = off + i * ent
            st_name  = self._u32(o + 0)
            st_value = self._u32(o + 4)
            sym_name = self._get_sym_name(st_name)
            if sym_name == name:
                return st_value
        return None

    def read_flash_device(self):
        """从 DevDscr 段解析 Keil FlashDevice 结构。"""
        sec = self.sections.get('DevDscr')
        if not sec:
            return None
        data = self.raw[sec['offset']: sec['offset'] + sec['size']]
        if len(data) < FLASHDEV_TOTAL:
            raise ValueError(f'DevDscr 段太小: {len(data)}')

        return {
            'ver':       struct.unpack_from('<H', data, FLASHDEV_OFF_VER)[0],
            'name':      data[FLASHDEV_OFF_NAME:FLASHDEV_OFF_NAME + 128].split(b'\x00', 1)[0].decode('ascii', errors='replace'),
            'dev_type':  struct.unpack_from('<H', data, FLASHDEV_OFF_TYPE)[0],
            'dev_adr':   struct.unpack_from('<I', data, FLASHDEV_OFF_ADR)[0],
            'sz_dev':    struct.unpack_from('<I', data, FLASHDEV_OFF_SZDEV)[0],
            'sz_page':   struct.unpack_from('<I', data, FLASHDEV_OFF_SZPAGE)[0],
            'prog_size': struct.unpack_from('<I', data, FLASHDEV_OFF_PROGSIZE)[0],
            'ram_addr':  struct.unpack_from('<I', data, FLASHDEV_OFF_RAMADR)[0],
            'ram_size':  struct.unpack_from('<I', data, FLASHDEV_OFF_RAMSIZE)[0],
        }


def _pick_chip_name(dev_name):
    """把 Keil 完整 device name 截短为 16 字节以内的精简名,便于 UI 显示。"""
    s = dev_name.replace('STM32F10x ', '').replace(' Flash', '').strip()
    if not s:
        s = 'STM32F10x'
    return s[:16]


def build_code_blob(elf):
    """拼 PrgCode + PrgData 成目标 RAM 镜像。Keil 装载时连续拷贝。"""
    code = elf.get_section_bytes('PrgCode') or b''
    data = elf.get_section_bytes('PrgData') or b''
    return code + data


def make_algo_header(elf, code_blob):
    fd = elf.read_flash_device()

    # 解析函数符号(Thumb 地址,bit0=1)
    init_off  = elf.get_symbol_value('Init')          or 0
    uninit_off= elf.get_symbol_value('UnInit')        or 0
    echip_off = elf.get_symbol_value('EraseChip')     or 0
    esec_off  = elf.get_symbol_value('EraseSector')   or 0
    ppage_off = elf.get_symbol_value('ProgramPage')   or 0

    # 兜底:符号缺失时用 0xFFFFFFFF
    def sym_or_default(v):
        return v if v else 0xFFFFFFFF

    init_off   = sym_or_default(init_off)
    uninit_off = sym_or_default(uninit_off)
    echip_off  = sym_or_default(echip_off)
    esec_off   = sym_or_default(esec_off)
    ppage_off  = sym_or_default(ppage_off)

    # flash_base / ram_size / buffer_size 从 FlashDevice 取。
    # ram_addr 字段在 Keil STM32F1xx_DFP 里常被填成 0x1F4 之类的魔数(算法内部偏移),
    # 与目标芯片实际 SRAM 基址无关。我们直接覆盖为目标芯片的 SRAM 起点。
    if fd:
        flash_base = fd['dev_adr']
        ram_size   = fd['ram_size']
        buf_size   = fd['prog_size']
        chip_name  = _pick_chip_name(fd['name'])
    else:
        flash_base = 0x08000000
        ram_size   = 0x00000400
        buf_size   = 0x00000064
        chip_name  = 'Unknown'

    # STM32F1 SRAM 固定起点
    ram_base = 0x20000000

    # 传统布局:buffer = ram_base + ram_size - buf_size
    # OPT 这种 buf_size > ram_size 的特殊算法(OPT 整片作为 page)做防御性回退。
    if buf_size > ram_size:
        buffer_addr = ram_base  # 没有真正的缓冲,直接用 SRAM 起点
    else:
        buffer_addr = ram_base + ram_size - buf_size

    name_bytes = chip_name.encode('ascii', errors='replace')
    if len(name_bytes) < 16:
        name_bytes = name_bytes + b'\x00' * (16 - len(name_bytes))
    name_bytes = name_bytes[:16]

    header = struct.pack('<IIIIII II III I 16s',
        ALGO_MAGIC,
        flash_base,
        ram_base,
        ram_size,
        buffer_addr,
        buf_size,
        init_off,
        uninit_off,
        echip_off,
        esec_off,
        ppage_off,
        len(code_blob),
        name_bytes,
    )
    assert len(header) == 64, f'header size = {len(header)}'
    return header, fd


def convert(flm_path, algo_dir):
    print(f'\n=== {os.path.basename(flm_path)} ===')
    elf = Elf(flm_path)
    code_blob = build_code_blob(elf)
    header, fd = make_algo_header(elf, code_blob)

    print(f'  PrgCode+PrgData = {len(code_blob)} bytes')
    if fd:
        print(f'  FlashDevice:    name={fd["name"]:24s} devAdr=0x{fd["dev_adr"]:08X} szDev={fd["sz_dev"]} szPage={fd["sz_page"]} progSize={fd["prog_size"]}')
        print(f'                  RamAdr=0x{fd["ram_addr"]:08X} RamSize={fd["ram_size"]}')

    syms = ['Init', 'UnInit', 'EraseChip', 'EraseSector', 'ProgramPage']
    for s in syms:
        v = elf.get_symbol_value(s)
        if v is not None:
            print(f'  symbol {s:14s} @ 0x{v:08X} (Thumb)')
        else:
            print(f'  symbol {s:14s} <missing>')

    # 输出文件名:STM32F10x_128.FLM -> STM32F10x_128.algo
    base = os.path.splitext(os.path.basename(flm_path))[0]
    out_path = os.path.join(algo_dir, base + '.algo')

    os.makedirs(algo_dir, exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(header)
        f.write(code_blob)

    print(f'  -> {out_path}  ({64 + len(code_blob)} bytes total)')
    return out_path


DEFAULT_FLM_DIR = r'C:\Keil_v5\ARM\PACK\Keil\STM32F1xx_DFP\2.4.1\Flash'
DEFAULT_OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'TF', 'algo')

ALL_FLMS = [
    'STM32F10x_16.FLM',
    'STM32F10x_128.FLM',
    'STM32F10x_512.FLM',
    'STM32F10x_1024.FLM',
    'STM32F10x_CL.FLM',
    'STM32F10x_OPT.FLM',
]


def main():
    ap = argparse.ArgumentParser(description='Convert Keil FLM to .algo format')
    ap.add_argument('files', nargs='*', help='FLM 文件路径')
    ap.add_argument('--all', action='store_true', help='转换 STM32F1xx_DFP 默认目录下的全部 6 个')
    ap.add_argument('--flm-dir', default=DEFAULT_FLM_DIR, help='Keil Flash 目录')
    ap.add_argument('--out-dir', default=DEFAULT_OUT_DIR, help='.algo 输出目录(默认 TF/algo/)')
    args = ap.parse_args()

    out_dir = os.path.abspath(args.out_dir)
    print(f'out_dir = {out_dir}')

    targets = []
    if args.all:
        for n in ALL_FLMS:
            targets.append(os.path.join(args.flm_dir, n))
    else:
        targets = list(args.files)

    if not targets:
        ap.error('需要指定 FLM 文件,或使用 --all')

    for p in targets:
        try:
            convert(p, out_dir)
        except Exception as e:
            print(f'  [FAIL] {p}: {e}')
            sys.exit(1)

    print('\nAll done.')


if __name__ == '__main__':
    main()