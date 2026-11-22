#!/usr/bin/env python3
"""The 3DS image's own symbols, as a file its RomFS carries.

symtab.py [--names REGEX] ELF OUT reads ELF's functions and objects by the
environment's NM, or else nm, those whose names REGEX matches whole where it is
given, and writes OUT, the table engine/3ds/symbols.c reads where the
PC's symbols.c reads /proc/self/exe: a .3dsx keeps no symbol table, and the
adapter finds some of the game's statics by name (ENGINE.md#starting-the-game).

The file is little-endian: the magic SA3S, the count of symbols, then one
entry of 16 bytes for each, its address, its size, 1 where it is code and 0
where it is an object, and the offset of its name among the names that follow,
each ending in a NUL.
"""
import os
import re
import struct
import subprocess
import sys

MAGIC = b'SA3S'
# nm's letters for code, and for data of every kind it gives a defined name
CODE = set('Tt')
DATA = set('DdBbRrVv')


def symbols(elf, wanted):
    """(address, size, code, name) for each function and object of elf whose
    name wanted matches, in the symbol table's order, as the PC's symbols.c
    adds them"""
    out = subprocess.run([os.environ.get('NM', 'nm'), '-S', '-p', '--defined-only', elf],
                         capture_output=True, text=True, check=True).stdout
    found = []
    for line in out.splitlines():
        fields = line.split()
        if len(fields) == 4:
            address, size, kind, name = fields
        elif len(fields) == 3:
            (address, kind, name), size = fields, '0'
        else:
            continue
        # ARM's mapping symbols, $a, $d and $t, name no function or object
        if kind not in CODE | DATA or name.startswith('$') or not int(address, 16):
            continue
        if wanted and not wanted.fullmatch(name):
            continue
        found.append((int(address, 16), int(size, 16), int(kind in CODE), name))
    return found


def main():
    args = sys.argv[1:]
    wanted = None
    if args[:1] == ['--names']:
        wanted = re.compile(args[1])
        args = args[2:]
    if len(args) != 2:
        sys.exit('usage: symtab.py [--names REGEX] ELF OUT')
    elf, out = args
    found = symbols(elf, wanted)
    names = bytearray()
    entries = bytearray()
    for address, size, code, name in found:
        entries += struct.pack('<IIII', address, size, code, len(names))
        names += name.encode() + b'\0'
    with open(out, 'wb') as f:
        f.write(MAGIC + struct.pack('<I', len(found)) + entries + names)
    print(f'{out}: {len(found)} symbols, {len(names)} bytes of names')


if __name__ == '__main__':
    main()
