import io
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'benchmarks' / 'l05_cpu'))
import client
import metrics
import audit_results

IMAGE = ROOT / 'benchmarks' / 'l05_cpu' / 'fixtures' / 'sparse.png'

class Response:
    status = 200
    def __init__(self, obj): self.body = json.dumps(obj).encode()
    def __enter__(self): return self
    def __exit__(self, *args): return None
    def read(self): return self.body

def reply(reason='stop', content='TEST'):
    return {'choices': [{'finish_reason': reason, 'message': {'content': content}}],
            'usage': {'prompt_tokens': 3, 'completion_tokens': 1}}

class ClientTest(unittest.TestCase):
    def test_protocol_and_success(self):
        def fake(request, timeout):
            payload = json.loads(request.data)
            self.assertEqual(request.full_url, 'http://127.0.0.1:8000/v1/chat/completions')
            self.assertEqual(payload['messages'][0]['content'][0]['type'], 'image_url')
            self.assertTrue(payload['messages'][0]['content'][0]['image_url']['url'].startswith('data:image/png;base64,'))
            self.assertEqual(payload['messages'][0]['content'][1]['text'], 'Read')
            self.assertIs(payload['cache_prompt'], False)
            self.assertEqual(timeout, 10)
            return Response(reply())
        with patch('urllib.request.urlopen', side_effect=fake):
            self.assertEqual(client.request_ocr(IMAGE, 'Read', 64, 10)['content'], 'TEST')

    def test_truncated_empty_and_missing_finish_are_failures(self):
        for data in (reply('length'), reply('stop',''), reply(None)):
            with self.subTest(data=data), patch('urllib.request.urlopen', return_value=Response(data)):
                with self.assertRaises(client.RequestFailure) as ctx:
                    client.request_ocr(IMAGE, 'Read', 64, 10)
                self.assertEqual(ctx.exception.kind, 'invalid_or_truncated')
                self.assertIsNotNone(ctx.exception.response_body)

    def test_non_loopback_refused(self):
        with self.assertRaises(ValueError):
            client.request_ocr(IMAGE, 'Read', 64, 10, 'http://example.com/v1/chat/completions')

    def test_transport_failure_retained(self):
        with patch('urllib.request.urlopen', side_effect=TimeoutError('simulated timeout')):
            with self.assertRaises(client.RequestFailure) as ctx:
                client.request_ocr(IMAGE, 'Read', 64, 10)
            self.assertEqual(ctx.exception.kind, 'transport_error')

class MetricsTest(unittest.TestCase):
    def test_cer_and_normalization(self):
        self.assertEqual(metrics.cer('Ａ B\nC', 'ABC'), 0)
        self.assertAlmostEqual(metrics.cer('ABCD', 'ABXD'), 0.25)
        self.assertEqual(metrics.summarize([1, 2, 3])['median_ms'], 2)
        with self.assertRaises(ValueError): metrics.cer('  ', 'text')

    def test_audit_rejects_cache_or_missing_cache_evidence(self):
        base = {'phase': 'first', 'status': 'completed_needs_quality_review',
                'http_status': 200, 'finish_reason': 'stop', 'content': 'A',
                'usage': {'prompt_tokens_details': {'cached_tokens': 0}}}
        self.assertEqual(audit_results.audit([base], 'A', 0), [])
        for cached in (1, None):
            changed = dict(base, usage={'prompt_tokens_details': {'cached_tokens': cached}})
            self.assertTrue(audit_results.audit([changed], 'A', 0))
        self.assertTrue(audit_results.audit([base], 'different', 0))

if __name__ == '__main__': unittest.main()
