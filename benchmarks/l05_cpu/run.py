"""Run a single-case, strictly sequential CPU OCR benchmark against a local service."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import threading
import time

from client import RequestFailure, request_ocr
from metrics import cer, summarize

ROOT = Path(__file__).resolve().parent
PROMPT = 'Extract the text in the image.'  # Existing board demo's text-region prompt.


def sample_linux(pid):
    sample = {'monotonic_s': time.monotonic()}
    try:
        status = Path(f'/proc/{pid}/status').read_text()
        for line in status.splitlines():
            for field in ('VmRSS', 'VmHWM'):
                if line.startswith(field + ':'):
                    sample[field + '_kib'] = int(line.split()[1])
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        sample['process_cpu_ticks'] = int(fields[11]) + int(fields[12])
    except (OSError, ValueError, IndexError):
        sample['process_unavailable'] = True
    try:
        for line in Path('/proc/meminfo').read_text().splitlines():
            if line.startswith('MemAvailable:'):
                sample['mem_available_kib'] = int(line.split()[1])
                break
    except OSError:
        pass
    temperatures = {}
    for zone in Path('/sys/class/thermal').glob('thermal_zone*'):
        try:
            temperatures[(zone/'type').read_text().strip()] = int((zone/'temp').read_text().strip())
        except (OSError, ValueError):
            pass
    sample['thermal_millidegrees_c'] = temperatures
    return sample


def monitor(pid, stop, samples):
    while not stop.is_set():
        samples.append(sample_linux(pid))
        stop.wait(1.0)
    samples.append(sample_linux(pid))


def run_case(args):
    manifest = json.loads((ROOT/'fixtures/manifest.json').read_text(encoding='utf-8'))
    selected = next((item for item in manifest if item['id'] == args.case), None)
    if not selected:
        raise ValueError('unknown case')
    image = ROOT/'fixtures'/selected['image']
    reference = (ROOT/'fixtures'/selected['reference']).read_text(encoding='utf-8')
    digest = hashlib.sha256(image.read_bytes()).hexdigest()
    if digest != selected['image_sha256']:
        raise ValueError('fixture hash mismatch')
    # Create a new directory, refusing to overwrite prior raw evidence.
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=False)
    config = {'case': selected, 'prompt': PROMPT, 'max_tokens': args.max_tokens,
              'timeout_seconds': args.timeout, 'warmups': args.warmups,
              'hot_requests': args.repeat, 'service_pid': args.pid, 'cache_prompt': False,
              'server_startup': 'record separately; first means first request for this image, not necessarily process cold start'}
    (out/'config.json').write_text(json.dumps(config, indent=2, ensure_ascii=False)+'\n', encoding='utf-8')
    stop, samples = threading.Event(), []
    thread = threading.Thread(target=monitor, args=(args.pid, stop, samples), daemon=True)
    thread.start()
    records = []
    try:
        phases = ['first'] + ['warmup'] * args.warmups + ['hot'] * args.repeat
        with (out/'requests.jsonl').open('w', encoding='utf-8', buffering=1) as log:
            for index, phase in enumerate(phases, 1):
                record = {'request_index': index, 'phase': phase, 'case': args.case}
                try:
                    response = request_ocr(image, PROMPT, args.max_tokens, args.timeout)
                    record.update(response)
                    record['status'] = 'completed_needs_quality_review'
                    record['cer_whitespace_removed_nfkc'] = cer(reference, response['content'])
                except RequestFailure as exc:
                    record.update(status=exc.kind, error=str(exc), elapsed_ms=exc.elapsed_ms,
                                  http_status=exc.http_status, response_body=exc.response_body)
                except Exception as exc:
                    record.update(status='client_error', error=str(exc))
                records.append(record)
                log.write(json.dumps(record, ensure_ascii=False)+'\n')
                print(f"{args.case} {phase} {index}: {record['status']}", flush=True)
                if record['status'] != 'completed_needs_quality_review':
                    # A timeout/transport error does not establish that the server stopped.
                    break
    finally:
        stop.set()
        thread.join()
        (out/'resource_samples.json').write_text(json.dumps(samples, indent=2)+'\n', encoding='utf-8')
    hot = [r for r in records if r['phase'] == 'hot' and r['status'] == 'completed_needs_quality_review']
    rss = [s['VmHWM_kib'] for s in samples if 'VmHWM_kib' in s]
    available = [s['mem_available_kib'] for s in samples if 'mem_available_kib' in s]
    cpus = []
    ticks_per_second = os.sysconf('SC_CLK_TCK') if hasattr(os, 'sysconf') else None
    for a, b in zip(samples, samples[1:]):
        delta = b['monotonic_s'] - a['monotonic_s']
        if ticks_per_second and delta > 0 and 'process_cpu_ticks' in a and 'process_cpu_ticks' in b:
            cpus.append((b['process_cpu_ticks']-a['process_cpu_ticks']) / ticks_per_second / delta * 100)
    summary = {'hot_latency': summarize([r['elapsed_ms'] for r in hot]),
               'hot_cer': [r['cer_whitespace_removed_nfkc'] for r in hot],
               'peak_process_hwm_kib': max(rss) if rss else None,
               'minimum_available_kib': min(available) if available else None,
               'peak_sampled_process_cpu_percent_one_core': max(cpus) if cpus else None,
               'completed_all': len(records) == len(phases) and all(r['status'] == 'completed_needs_quality_review' for r in records),
               'quality_review': 'pending human review; CER alone does not prove completeness'}
    (out/'summary.json').write_text(json.dumps(summary, indent=2, ensure_ascii=False)+'\n', encoding='utf-8')
    print(json.dumps(summary, ensure_ascii=False), flush=True)
    return 0 if summary['completed_all'] else 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--case', choices=['sparse','medium','dense'], required=True)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--max-tokens', type=int, default=512)
    parser.add_argument('--warmups', type=int, default=1)
    parser.add_argument('--repeat', type=int, default=5)
    args = parser.parse_args()
    if args.pid <= 0 or args.timeout <= 0 or args.max_tokens <= 0 or args.warmups < 0 or args.repeat < 0:
        parser.error('invalid positive argument')
    return run_case(args)

if __name__ == '__main__':
    raise SystemExit(main())
