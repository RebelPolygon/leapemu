#!/usr/bin/env python3
"""Compare a leapemu trace (--trace-format mame) against a MAME oracle trace.

Both files: one line per instruction,
  PC STATUS32 LP_COUNT LP_START LP_END r0..r31 | disasm
(values = state before the instruction). Either file may be gzipped.

Reports the first line where any field differs, with context, and which
fields differ. Use --ignore to skip fields (e.g. --ignore status32).
"""
import argparse, gzip, sys

NAMES = ['pc', 'status32', 'lp_count', 'lp_start', 'lp_end'] + \
        [f'r{i}' for i in range(26)] + ['gp', 'fp', 'sp', 'ilink1', 'ilink2', 'blink']

def lines(path):
    op = gzip.open if path.endswith('.gz') else open
    with op(path, 'rt', errors='replace') as f:
        for line in f:
            # MAME annotates interrupts with "(interrupted at X, IRQ N)" lines.
            if not line.strip() or line.lstrip().startswith('('):
                continue
            head, _, dis = line.partition('|')
            yield [int(x, 16) for x in head.split()], dis.strip()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ours'); ap.add_argument('oracle')
    ap.add_argument('--ignore', action='append', default=[], help='field name to ignore')
    ap.add_argument('--context', type=int, default=6)
    a = ap.parse_args()
    ign = {NAMES.index(n) for n in a.ignore}
    hist = []
    n = 0
    for (fo, do), (fm, dm) in zip(lines(a.ours), lines(a.oracle)):
        n += 1
        if len(fo) < len(NAMES) or len(fm) < len(NAMES):
            print(f'malformed line {n}: ours={len(fo)} fields, oracle={len(fm)} fields: {dm[:80]!r}')
            return 2
        diff = [i for i in range(len(NAMES)) if i not in ign and fo[i] != fm[i]]
        if diff:
            print(f'DIVERGENCE at line {n}')
            for k, (h_o, h_d) in enumerate(hist[-a.context:]):
                print(f'   {h_o[0]:08x}  {h_d}')
            print(f'>> {fo[0]:08x}  ours:   {do}')
            print(f'>> {fm[0]:08x}  oracle: {dm}')
            for i in diff:
                print(f'   {NAMES[i]:9s} ours={fo[i]:08x} oracle={fm[i]:08x}')
            return 1
        hist.append((fo, do))
        if len(hist) > 64: hist = hist[-a.context:]
    print(f'{n} lines identical')
    return 0

sys.exit(main())
