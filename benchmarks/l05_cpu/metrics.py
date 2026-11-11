"""Character error rate and descriptive statistics; no percentile claims for small samples."""
import statistics
import unicodedata


def normalize(text):
    return ''.join(c for c in unicodedata.normalize('NFKC', text) if not c.isspace())


def cer(reference, hypothesis):
    ref, hyp = normalize(reference), normalize(hypothesis)
    if not ref:
        raise ValueError('empty normalized reference')
    prev = list(range(len(hyp) + 1))
    for i, a in enumerate(ref, 1):
        cur = [i]
        for j, b in enumerate(hyp, 1):
            cur.append(min(cur[-1] + 1, prev[j] + 1, prev[j-1] + (a != b)))
        prev = cur
    return prev[-1] / len(ref)


def summarize(values):
    if not values:
        return None
    return {'count': len(values), 'mean_ms': statistics.mean(values),
            'median_ms': statistics.median(values), 'min_ms': min(values), 'max_ms': max(values)}
