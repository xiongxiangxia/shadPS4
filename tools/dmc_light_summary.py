#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import collections
import csv
import json
import struct
from pathlib import Path


def floating(value):
    return struct.unpack('<f', struct.pack('<I', value & 0xffffffff))[0]


def summarize(path):
    with path.open(encoding='utf-8-sig') as stream:
        metadata = []

        def lines():
            for line in stream:
                if line.startswith('#'):
                    value = line.strip()
                    if value not in metadata:
                        metadata.append(value)
                else:
                    yield line

        rows = list(csv.DictReader(lines()))

    def p(row, column):
        return int(row[f'p{column}'])

    counts = collections.Counter(r['kind'] for r in rows)
    markers = [i for i, r in enumerate(rows) if r['kind'] == 'DMC_LIGHT_CAPTURE']
    captures = []
    for index, begin in enumerate(markers):
        end = markers[index + 1] if index + 1 < len(markers) else len(rows)
        segment = rows[begin:end]
        origin = int(rows[begin]['monotonic_us'])
        ticks = [r for r in segment if r['kind'] == 'DMC_LIGHT_TICK']
        costs = [r for r in segment if r['kind'] == 'DMC_LIGHT_COST']
        latest = {}
        for row in costs:
            latest[row['thread']] = row
        deltas = sorted(floating(p(r, 3)) for r in ticks)
        hooks = p(rows[begin], 1)
        captures.append({
            'hooks': hex(hooks),
            'all_hooks_installed': hooks == 0x3ff,
            'counts': dict(collections.Counter(r['kind'] for r in segment if r['kind'].startswith('DMC_LIGHT'))),
            'timeline_delta_ms': {
                'p50': deltas[len(deltas) // 2] * 1000 if deltas else None,
                'max': max(deltas) * 1000 if deltas else None,
                'over_50ms': sum(v > .05 for v in deltas)},
            'measured_probe_ms': sum(p(r, 1) for r in latest.values()) / 1e6,
            'max_measured_probe_phase_ms': max((p(r, 2) for r in costs), default=0) / 1e6,
            'threads_with_cost_reports': len(latest),
            'replays': [
                {'ms': (int(r['monotonic_us']) - origin) / 1000, 'thread': r['thread'],
                 'graph': hex(p(r, 1)), 'before': hex(p(r, 3)), 'after': hex(p(r, 4)),
                 'result': p(r, 5), 'parent': p(r, 0)}
                for r in segment if r['kind'] == 'DMC_LIGHT_GRAPH' and p(r, 7)],
            'note': 'Cost reports cover completed instrumentation phases, excluding original guest calls; '
                    'they are partial measurements, not total emulator overhead. Timeline samples may '
                    'include several player actions per frame. No charge-start/PS4 comparison is inferred.'})
    return {'source': str(path), 'metadata': metadata, 'rows': len(rows),
            'light_limit_rows': counts['DMC_LIGHT_LIMIT'], 'captures': captures}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('csv', type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.csv), indent=2, ensure_ascii=False))
