#!/usr/bin/env python3
"""Rebase the game's RAM addresses for the 3DS target.

The game names its RAM by number in a few places: the loader's work areas, the
overlay table's words, the boot headers, the stack bounds and the heap. A PC
run maps RAM at 0x0c000000 and its SH-4 P1 view at 0x8c000000; a .3dsx cannot
map either, and the only fixed addresses it gets are those of its own image.
The target therefore reserves 16 MiB past its image, at --base, 0x04000000 by
default, and every address the game writes as 0x0cXXXXXX, 0x8cXXXXXX or
0xacXXXXXX must move there.

The pass copies sdk/ and loader/ into TREE, as marks.py copies src/, modules/
and events/, and then rewrites those literals *in code* under TREE, keeping the
low 24 bits. It reads comments and string literals past: the sources cite
original addresses in thousands of comments, and a comment is neither the
game's code nor its data. The report, TREE/rambase.txt, names each file it
changed and the count. A tree the pass has already rebased changes by nothing,
so a second run writes the same report.

The disc's data needs no pass: the content expresses pointers into RAM as
names, and the words under build/gen/ that look like addresses are font and
camera data rather than pointers.
"""
import argparse
import re
import shutil
import sys
from pathlib import Path

SIZE = 0x01000000
NEW = 0x04000000
VIEW = re.compile(r'\b0[xX](0[cC]|8[cC]|[aA][cC])([0-9a-fA-F]{6})\b')
# the SH-4's registers and the Dreamcast's devices that the game's C reaches
# by a pointer cast of a constant: the on-chip registers, the store queues,
# the system bus, PVR, AICA and the TA's FIFO. the PC maps zeros that take
# writes there, so a read of a timer finds no time gone; a .3dsx can map none
# of them, so a cast of one points into sa3ds_devices, a megabyte of zeros the
# target's runner gives, at the address's low 20 bits
DEVICE = re.compile(r'(\(\s*(?:volatile\s+|const\s+)*\w+(?:\s+\w+)*\s*\*\s*\)\s*)'
                    r'0[xX]((?:[aA][0-7]|[eE][0-3]|[fF][0-9a-fA-F]|7[cC]|1[01])[0-9a-fA-F]{6})\b')
DEVICE_BYTES = 0x100000
# the store queues' area, which Ninja's and the game's drawers write their
# vertices through as 0xe0000000 ORed with a 64 MB offset. the target points
# the area at sa3ds_store_queue, aligned to its own 256 KB, and keeps the
# offset's low 18 bits, so a write lands there whatever the list it is for
QUEUE = re.compile(r'(#define\s+(?:SQ_BASE|STORE_QUEUE_BASE|SQ_AREA)\s+)0[xX][eE]0000000[uUlL]*\b')
QUEUE_MASK = re.compile(r'(#define\s+(?:SQ_ADDRESS|SQ_OFFSET)\s+)0[xX]03[fF]{6}[uUlL]*\b')
# main memory's end, which sbinit.c ORs into the region _BSG_END lies in
RAM_END = re.compile(r'\b0[xX]0[dD]000000\b')
COPIED = ('sdk', 'loader')
# newlib declares `char *strupr(char *)` and Shinobi's ninja.h `void
# strupr(char *)`, so a host compile that sees both stops on the two types.
# the copy renames Ninja's, and the calls and the one definition follow it
RENAMED = {
    'sdk/include/ninja.h': (('void strupr(char *s);', 'void sa1_strupr(char *s);'),),
    'sdk/ninja/dummy.c': (('void strupr(char *s)', 'void sa1_strupr(char *s)'),),
    'sdk/ninja/njLoadTexturePvmFile.c': (('strupr(ext);', 'sa1_strupr(ext);'),),
    'sdk/ninja/njTexinner.c': (('strupr(ext);', 'sa1_strupr(ext);'),),
    # card.c aliases two struct members to global names with gas directives:
    # the ARM assembler writes a type as %object and an immediate as %cN,
    # where x86's gas takes @object and %aN
    'src/save/card.c': (('@object', '%object'),),
    # syCacheICI reaches its function through the SH-4's uncached P2 view,
    # its address ORed with 0xa0000000, which a .3dsx cannot map. the ARM11
    # has no cache line of the game's to drop, so the call is a plain one
    'sdk/include/shinobi.h':(('#define syCacheICI() ((void (*)(void))(0xa0000000 | (Uint32)syCacheICI_P2_))()',
                               '#define syCacheICI() syCacheICI_P2_()'),),
}
# the alias itself: `.set light_ry, sources_file + 0x5a0` needs the assembler to
# know another object's symbol, which ARM's gas does not. the target drops the
# alias, and a module that imports the name resolves it through the loader
OMITTED = {'src/save/card.c': re.compile(r'__asm__\(\s*(?:"(?:[^"\\]|\\.)*"\s*)+\);')}


def segments(text):
    """TEXT as runs of (is_code, text): comments and strings are not code."""
    out = []
    code = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            if code:
                out.append((True, ''.join(code)))
                code = []
            end = text.find('\n', i)
            end = n if end < 0 else end
            out.append((False, text[i:end]))
            i = end
        elif c == '/' and i + 1 < n and text[i + 1] == '*':
            if code:
                out.append((True, ''.join(code)))
                code = []
            end = text.find('*/', i + 2)
            end = n if end < 0 else end + 2
            out.append((False, text[i:end]))
            i = end
        elif c in '"\'':
            if code:
                out.append((True, ''.join(code)))
                code = []
            quote = c
            j = i + 1
            while j < n and text[j] != quote:
                if text[j] == '\\':
                    j += 1
                j += 1
            j = min(j + 1, n)
            out.append((False, text[i:j]))
            i = j
        else:
            code.append(c)
            i += 1
    if code:
        out.append((True, ''.join(code)))
    return out


def rebase(text, new):
    """TEXT with each RAM literal in its code moved to NEW, each device's
    cast moved into sa3ds_devices, and the count."""
    count = 0

    def one(match):
        nonlocal count
        count += 1
        return f'0x{int(match.group(0), 16) & (SIZE - 1) | new:08x}'

    def device(match):
        nonlocal count
        count += 1
        return f'{match[1]}((char *)sa3ds_devices + 0x{int(match[2], 16) & (DEVICE_BYTES - 1):05x})'

    def queue(match):
        nonlocal count
        count += 1
        return f'{match[1]}((Uint32)sa3ds_store_queue)'

    def queue_mask(match):
        nonlocal count
        count += 1
        return f'{match[1]}0x0003ffff'

    def ram_end(match):
        nonlocal count
        count += 1
        return f'0x{new + SIZE:08x}'

    def code(part):
        part = QUEUE_MASK.sub(queue_mask, QUEUE.sub(queue, part))
        return RAM_END.sub(ram_end, DEVICE.sub(device, VIEW.sub(one, part)))

    return ''.join((code(part) if is_code else part) for is_code, part in segments(text)), count


# a RAM address the disc's data casts to a pointer, as the camera's layouts
# name the areas their files load at
CAST = re.compile(r'(\(\s*(?:const\s+|volatile\s+)*(?:struct\s+)?\w+(?:\s+\w+)*\s*\*\s*\)\s*)'
                  r'0[xX](?:0[cC]|8[cC]|[aA][cC])([0-9a-fA-F]{6})\b')


def casts(path, new):
    """rebase each RAM address path, a unit's preprocessed C, casts to a
    pointer, and each device's: the disc's data under build/gen/ holds them
    beside words that only look like addresses, which stay"""
    text = Path(path).read_text(errors='surrogateescape')
    count = 0

    def one(match):
        nonlocal count
        count += 1
        return f'{match[1]}0x{int(match[2], 16) | new:08x}'

    def device(match):
        nonlocal count
        count += 1
        return f'{match[1]}((char *)sa3ds_devices + 0x{int(match[2], 16) & (DEVICE_BYTES - 1):05x})'

    moved = ''.join((DEVICE.sub(device, CAST.sub(one, part)) if is_code else part)
                    for is_code, part in segments(text))
    if count:
        Path(path).write_text(moved, errors='surrogateescape')
    return count


def copied(tree):
    """sdk/ and loader/, into TREE, so the pass can rewrite them."""
    for name in COPIED:
        source = Path(name)
        if not source.is_dir():
            continue
        for path in source.rglob('*'):
            if not path.is_file():
                continue
            target = Path(tree) / path
            # a copy newer than its source is this pass's own: it was rebased,
            # and copying again would only make every object stale
            if target.exists() and target.stat().st_mtime >= path.stat().st_mtime:
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
        for stale in (Path(tree) / name).rglob('*'):
            if stale.is_file() and not (Path(name) / stale.relative_to(Path(tree) / name)).exists():
                stale.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--base', type=lambda s: int(s, 0), default=NEW)
    parser.add_argument('--casts', action='store_true',
                        help='rebase the pointer casts of TREE, one preprocessed unit, alone')
    parser.add_argument('tree')
    args = parser.parse_args()
    if args.casts:
        casts(args.tree, args.base)
        return
    if args.base & (SIZE - 1):
        sys.exit(f'--base {args.base:#x}: not a multiple of 16 MiB')
    tree = Path(args.tree)
    if not tree.is_dir():
        sys.exit(f'{tree}: no such tree')
    copied(tree)
    report = []
    total = 0
    for path in sorted(tree.rglob('*')):
        if path.suffix not in ('.c', '.h', '.inc') or not path.is_file():
            continue
        text = path.read_text(errors='surrogateescape')
        moved, count = rebase(text, args.base)
        renamed = RENAMED.get(str(path.relative_to(tree)), ())
        for old, new in renamed:
            if old not in moved:
                if new in moved:
                    continue
                sys.exit(f'{path}: no {old!r} to rename')
            moved = moved.replace(old, new)
        omitted = OMITTED.get(str(path.relative_to(tree)))
        if omitted is not None:
            moved, dropped = omitted.subn('/* the alias this target does not make */', moved)
            if not dropped and 'the alias this target does not make' not in moved:
                sys.exit(f'{path}: no assembler alias to drop')
        if count or renamed or omitted is not None:
            if path.read_text(errors='surrogateescape') != moved:
                path.write_text(moved, errors='surrogateescape')
            report.append(f'{count:4d} {path.relative_to(tree)}')
            total += count
    (tree / 'rambase.txt').write_text('# each file this pass rebased, and how many literals\n' +
                                      '\n'.join(report) + ('\n' if report else ''))
    print(f'rambase: {total} literals in {len(report)} files at 0x{args.base:08x}')


if __name__ == '__main__':
    main()
