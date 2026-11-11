"""Check request evidence, including whether prompt caching was actually disabled."""
import argparse
import json
from pathlib import Path

from metrics import cer


def audit(records, reference, expected_hot):
    issues = []
    phases = [record.get('phase') for record in records]
    if len(phases) < 1 or phases[0] != 'first' or phases.count('first') != 1:
        issues.append('expected exactly one initial first request')
    if phases.count('hot') != expected_hot:
        issues.append(f'expected {expected_hot} hot requests, got {phases.count("hot")}')
    for i, record in enumerate(records, 1):
        if record.get('status') != 'completed_needs_quality_review' or record.get('http_status') != 200:
            issues.append(f'request {i}: request did not complete successfully')
        if record.get('finish_reason') != 'stop':
            issues.append(f'request {i}: finish reason is not stop')
        cached = (record.get('usage') or {}).get('prompt_tokens_details', {}).get('cached_tokens')
        if cached != 0:
            issues.append(f'request {i}: prompt cache not proven disabled (cached_tokens={cached!r})')
        content = record.get('content')
        if not isinstance(content, str) or not content.strip():
            issues.append(f'request {i}: empty content')
        elif cer(reference, content) != 0:
            issues.append(f'request {i}: OCR differs from normalized reference (CER={cer(reference, content):.6f})')
    return issues


def main():
    p = argparse.ArgumentParser()
    p.add_argument('requests', type=Path)
    p.add_argument('reference', type=Path)
    p.add_argument('--expected-hot', type=int, default=5)
    args = p.parse_args()
    if args.expected_hot < 0:
        p.error('--expected-hot must be nonnegative')
    records = [json.loads(line) for line in args.requests.read_text(encoding='utf-8').splitlines() if line.strip()]
    issues = audit(records, args.reference.read_text(encoding='utf-8'), args.expected_hot)
    print(json.dumps({'requests': len(records), 'issues': issues,
                      'note': 'Automated checks do not replace human quality review.'}, ensure_ascii=False, indent=2))
    return 1 if issues else 0


if __name__ == '__main__':
    raise SystemExit(main())
