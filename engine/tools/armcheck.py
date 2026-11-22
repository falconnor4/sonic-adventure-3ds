#!/usr/bin/env python3
"""Compile each game source the 3DS target builds, for the ARM11, and report
what fails, by kind.

The sources are the ones engine/mk/3ds.mk builds: the game's C as each image's
splits.txt places it, and NONMATCHING_C. Each compiles from the marked copy
marks.py writes, as the target's rule does, with the compiler, include
directories and flags the rule gives, which make passes here.

A source that fails is classified once, by its first error:

- **disc**  an include the disc's data writes under build/gen/. make pc or make
  dc writes those, so the source waits on the disc rather than on the target.
- **error** the compiler's own. A run with one exits 1, so the check fails
  where the target cannot build a source the disc would not change.

The report, build/3ds/check.txt, names every source that fails and why, and
counts each kind. --jobs, 8 by default, compiles that many at a time.
"""
import argparse
import collections
import concurrent.futures
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))

GAME = ('src/', 'modules/', 'events/')


def sources():
    """The game's C, as splits.txt places it, and NONMATCHING_C."""
    found = set()
    for splits in Path('config').glob('*/splits.txt'):
        for line in splits.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            name = line.split()[0]
            if name.endswith('.c') and name.startswith(GAME):
                found.add(name)
    text = Path('config/compilers.mk').read_text()
    joined = re.sub(r'\\\n', ' ', text)
    for line in joined.splitlines():
        if line.startswith('NONMATCHING_C'):
            found.update(w for w in line.split('=')[1].split() if w.endswith('.c'))
    return sorted(found)


def compile_one(cc, includes, flags, tree, name):
    """(source, kind, first error), kind 'ok', 'disc' or 'error'."""
    source = Path(tree) / name if name.startswith(GAME) else Path(name)
    if not source.exists():
        source = Path(name)
    command = [cc, '-fsyntax-only', *flags, *includes, str(source)]
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return name, 'error', 'timed out'
    if done.returncode == 0:
        return name, 'ok', ''
    lines = [line for line in done.stderr.splitlines() if ' error:' in line or 'fatal error:' in line]
    first = lines[0] if lines else (done.stderr.splitlines() or ['no message'])[0]
    first = re.sub(r'^.*?:\d+:\d+: ', '', first)
    kind = 'disc' if 'build/gen/' in first or re.search(r'/[^/]+\.inc:', first) else 'error'
    return name, kind, first


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--cc', required=True)
    parser.add_argument('--include', action='append', default=[])
    parser.add_argument('--flag', action='append', default=[])
    parser.add_argument('--tree', default='build/3ds/src')
    parser.add_argument('--report', default='build/3ds/check.txt')
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()

    names = sources()
    includes = [f'-I{d}' for d in args.include]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(lambda n: compile_one(args.cc, includes, args.flag, args.tree, n), names))

    counts = collections.Counter(kind for _, kind, _ in results)
    report = Path(args.report)
    report.parent.mkdir(parents=True, exist_ok=True)
    with report.open('w') as out:
        out.write('# the 3DS target\'s sources, and what each compiled to\n')
        out.write(f'# {counts["ok"]} of {len(results)} compile, {counts["disc"]} wait on the disc, '
                  f'{counts["error"]} fail\n')
        for name, kind, why in results:
            if kind != 'ok':
                out.write(f'{kind:5s} {name}: {why}\n')

    print(f'3ds-check: {counts["ok"]} of {len(results)} sources compile for the ARM11, '
          f'{counts["disc"]} wait on the disc, {counts["error"]} fail')
    if counts['error']:
        for name, kind, why in results:
            if kind == 'error':
                print(f'  {name}: {why}')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
