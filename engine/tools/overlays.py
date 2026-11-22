#!/usr/bin/env python3
"""The 3DS image's linker script, with the loader's programs overlaid.

overlays.py TEMPLATE OUT RAM PROGRAM... writes OUT from TEMPLATE, the 3DS's
script (engine/3ds/sa3ds.ld.in), with @RAM@ as RAM, the game's RAM, and the
programs' sections in place of the line @PROGRAMS@. Each PROGRAM's data and
bss come from DIR/program-PROGRAM.o, where DIR is OUT's directory, by the
environment's SIZE, or else size.

The PC links every program at once, its data in memory from the start. The
3DS can't hold that: the programs' data is some 60 MB. On the Dreamcast a
program loads at its image's origin over the last one there, and two images at
one origin are never live together, as the memory contract's colive says (the
script fails where it finds two that are). So the 3DS gives each origin a
region, as long as the largest of its programs' data and bss, and links each
program's data and bss at its region's start. A program's data is placed for
the file alone, past the image's segments, in a segment the .3dsx leaves out:
3ds.mk copies each program's data into the image's RomFS, and engine/3ds's
loads.c reads it back into the region as the game's loader reads the
program's file.
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import images  # noqa: E402
import memory  # noqa: E402

# a region's and a bss's alignment: a cache line of the ARM11's
ALIGN = 32
# where the programs' data lies in the ELF file, an address no segment of the
# image reaches
FILE_BASE = 0x40000000


def up(n, to=ALIGN):
    return (n + to - 1) & ~(to - 1)


def sections(path):
    """the sizes of path's sections, by name"""
    out = subprocess.run([os.environ.get('SIZE', 'size'), '-A', str(path)], capture_output=True, text=True,
                         check=True).stdout
    found = {}
    for line in out.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1].isdigit():
            found[fields[0]] = int(fields[1])
    return found


def main():
    if len(sys.argv) < 4:
        sys.exit('usage: overlays.py TEMPLATE OUT RAM PROGRAM...')
    template, out, ram, *programs = sys.argv[1:]
    where = Path(out).parent
    contract = memory.contract(images.images())
    groups = {}
    for p in programs:
        groups.setdefault(contract.origins[p] & 0x1fffffff, []).append(p)
    for members in groups.values():
        for a in members:
            for b in members:
                if a < b and b in contract.colive[a]:
                    sys.exit(f'{a} and {b} load at one origin and are live together: no region holds both')
    sizes = {}
    for p in programs:
        s = sections(where / f'program-{p}.o')
        sizes[p] = (s.get(f'sa1_data_{p}', 0), s.get(f'sa1_bss_{p}', 0))
    lines = ['\t/* the programs\' regions, one for each origin of the Dreamcast\'s, which',
             '\t   the loader\'s reads fill as on the console */',
             f'\t. = ALIGN({ALIGN});', '\t__sa1_programs = .;']
    placed = []
    at = 0
    for origin, members in sorted(groups.items()):
        room = max(up(sizes[p][0]) + up(sizes[p][1]) for p in members)
        placed.append((origin, at, room, members))
        at += up(room)
    lines.append(f'\t.sa1_programs (NOLOAD) : {{ . += {at:#x}; }} : data')
    lines.append('\t__sa1_programs_end = .;')
    stored = FILE_BASE
    for origin, offset, room, members in placed:
        lines.append(f'\t/* the region of origin {origin:#010x}: {room} bytes, {len(members)} programs */')
        for p in members:
            data, bss = sizes[p]
            lines.append(f'\tsa1_data_{p} __sa1_programs + {offset:#x} : AT({stored:#x}) '
                         f'{{ KEEP(*(sa1_data_{p})) }} : programs')
            lines.append(f'\tsa1_bss_{p} ALIGN(ADDR(sa1_data_{p}) + SIZEOF(sa1_data_{p}), {ALIGN}) (NOLOAD) : '
                         f'AT({stored + up(data, 0x10):#x}) {{ *(sa1_bss_{p}) }} : programs')
            stored += up(data, 0x10)
    lines.append('\t. = __sa1_programs_end;')
    text = Path(template).read_text().replace('@RAM@', ram)
    if '@PROGRAMS@' not in text:
        sys.exit(f'{template}: no @PROGRAMS@ line')
    Path(out).write_text(text.replace('@PROGRAMS@', '\n'.join(lines)))
    print(f'{out}: {len(programs)} programs in {len(placed)} regions, {at} bytes')


if __name__ == '__main__':
    main()
