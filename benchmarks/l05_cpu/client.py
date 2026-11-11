"""Minimal loopback-only client for the board's existing Xiaomi OCR demo protocol."""
import base64
import json
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


class RequestFailure(Exception):
    def __init__(self, kind, message, elapsed_ms, response_body=None, http_status=None):
        super().__init__(message)
        self.kind = kind
        self.elapsed_ms = elapsed_ms
        self.response_body = response_body
        self.http_status = http_status


def request_ocr(image, prompt, max_tokens, timeout, url='http://127.0.0.1:8000/v1/chat/completions'):
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != 'http' or parsed.hostname not in ('127.0.0.1', 'localhost', '::1') or parsed.username:
        raise ValueError('only a local HTTP endpoint is allowed')
    data = Path(image).read_bytes()
    if not data.startswith(b'\x89PNG\r\n\x1a\n'):
        raise ValueError('expected a PNG image')
    encoded = base64.b64encode(data).decode('ascii')
    payload = {'model': 'xiaomi-ocr-0', 'messages': [{'role': 'user', 'content': [
        {'type': 'image_url', 'image_url': {'url': 'data:image/png;base64,' + encoded}},
        {'type': 'text', 'text': prompt}]}], 'temperature': 0.0, 'max_tokens': max_tokens,
        'stream': False, 'top_p': 0.8, 'presence_penalty': 0.0, 'repetition_penalty': 1.0,
        'chat_template_kwargs': {'enable_thinking': False}, 'cache_prompt': False}
    request = urllib.request.Request(url, data=json.dumps(payload).encode(),
        headers={'Content-Type': 'application/json', 'Accept': 'application/json'}, method='POST')
    start = time.monotonic()
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw, status = response.read().decode('utf-8'), response.status
    except urllib.error.HTTPError as exc:
        raise RequestFailure('http_error', str(exc), (time.monotonic()-start)*1000,
                             exc.read().decode('utf-8', 'replace'), exc.code) from exc
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise RequestFailure('transport_error', str(exc), (time.monotonic()-start)*1000) from exc
    elapsed = (time.monotonic()-start)*1000
    try:
        result = json.loads(raw)
        choice = result['choices'][0]
        content = choice['message']['content']
        if isinstance(content, list):
            content = ''.join(part.get('text', '') for part in content if isinstance(part, dict))
        if not isinstance(content, str) or not content.strip():
            raise ValueError('empty result')
        if choice.get('finish_reason') != 'stop':
            raise ValueError('non-stop or missing finish_reason: ' + str(choice.get('finish_reason')))
    except (ValueError, KeyError, IndexError, TypeError) as exc:
        raise RequestFailure('invalid_or_truncated', str(exc), elapsed, raw, status) from exc
    return {'http_status': status, 'elapsed_ms': elapsed, 'content': content,
            'finish_reason': choice['finish_reason'], 'usage': result.get('usage'), 'raw': result}
