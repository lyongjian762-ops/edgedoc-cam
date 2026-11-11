"""Owner-controlled local server lifecycle and basic low-memory emergency stop."""
import argparse
import json
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


def available_kib():
    for line in Path('/proc/meminfo').read_text().splitlines():
        if line.startswith('MemAvailable:'):
            return int(line.split()[1])
    raise RuntimeError('MemAvailable unavailable')


def stopped_signal(signum, frame):
    raise KeyboardInterrupt()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True, help='new directory for this run')
    parser.add_argument('--script', default=str(Path.home()/'ai/xiaomi-ocr-0/run-model.sh'))
    parser.add_argument('--min-available-mib', type=int, default=512)
    args = parser.parse_args()
    with socket.socket() as probe:
        if probe.connect_ex(('127.0.0.1', 8000)) == 0:
            raise RuntimeError('port 8000 is already in use; refusing to start')
    if available_kib() < args.min_available_mib * 1024:
        raise RuntimeError('insufficient available memory')
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=False)
    signal.signal(signal.SIGTERM, stopped_signal)
    signal.signal(signal.SIGHUP, stopped_signal)
    process = None
    start = time.monotonic()
    with (out/'server.log').open('w', encoding='utf-8') as log, (out/'guard.jsonl').open('w', encoding='utf-8', buffering=1) as events:
        def record(event):
            events.write(json.dumps(event)+'\n')
            print(json.dumps(event), flush=True)
        try:
            # Keep terminal Ctrl-C aimed at the guard; forward exactly one signal.
            process = subprocess.Popen([args.script], stdin=subprocess.DEVNULL, stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            record({'event':'started', 'pid':process.pid, 'available_kib':available_kib()})
            ready = False
            while process.poll() is None:
                memory = available_kib()
                if memory < args.min_available_mib * 1024:
                    record({'event':'low_memory_stop', 'available_kib':memory})
                    break
                if not ready:
                    if time.monotonic() - start > 180:
                        record({'event':'startup_timeout'})
                        break
                    try:
                        with urllib.request.urlopen('http://127.0.0.1:8000/health', timeout=2) as response:
                            if response.status == 200:
                                ready = True
                                record({'event':'ready', 'pid':process.pid, 'startup_ms':(time.monotonic()-start)*1000,
                                        'available_kib':memory})
                    except (OSError, urllib.error.URLError):
                        pass
                time.sleep(1)
            else:
                record({'event':'server_exited', 'returncode':process.returncode})
        except KeyboardInterrupt:
            record({'event':'stop_requested'})
        finally:
            if process is not None and process.poll() is None:
                process.send_signal(signal.SIGINT)
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                record({'event':'server_stopped', 'pid':process.pid, 'returncode':process.returncode})
    return 0

if __name__ == '__main__':
    sys.exit(main())
