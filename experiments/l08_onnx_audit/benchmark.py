import csv
import random
import statistics
import time
import numpy as np
from compare_outputs import session


def run(models, inputs, output):
    raw, summaries = [], {}
    for track, path in models.items():
        sessions, init = {}, {}
        for level in ('disabled', 'basic', 'all') if track == 'tiny' else ('disabled', 'basic'):
            start = time.perf_counter_ns(); sessions[level] = session(path, level); init[level] = time.perf_counter_ns() - start
            for _ in range(30): sessions[level].run(None, {'x': inputs[track]})
        order = list(sessions)
        rng = random.Random(42)
        for block in range(3):
            rng.shuffle(order)
            for level in order:
                for i in range(100):
                    start = time.perf_counter_ns(); sessions[level].run(None, {'x': inputs[track]}); elapsed = time.perf_counter_ns() - start
                    raw.append({'track': track, 'level': level, 'block': block, 'iteration': i, 'elapsed_ns': elapsed})
        for level in sessions:
            samples = [x['elapsed_ns'] for x in raw if x['track'] == track and x['level'] == level]
            summaries[f'{track}_{level}'] = {'init_ns': init[level], 'count': len(samples), 'median_ns': statistics.median(samples),
                'mean_ns': statistics.mean(samples), 'min_ns': min(samples), 'max_ns': max(samples),
                'p90_ns': float(np.percentile(samples, 90)), 'stdev_ns': statistics.stdev(samples),
                'block_medians_ns': [statistics.median(x['elapsed_ns'] for x in raw if x['track']==track and x['level']==level and x['block']==b) for b in range(3)]}
    with (output / 'benchmark_raw.csv').open('w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=['track','level','block','iteration','elapsed_ns']); w.writeheader(); w.writerows(raw)
    return summaries
