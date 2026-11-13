import json
import shutil
from compare_outputs import session


def profile(path, x, level, destination):
    s = session(path, level, profile=True)
    for _ in range(5): s.run(None, {'x': x})
    source = s.end_profiling()
    shutil.move(source, destination)
    events = json.loads(destination.read_text(encoding='utf-8'))
    return [{'name': e.get('name'), 'duration_us': e.get('dur'), 'op_type': e.get('args', {}).get('op_name'),
             'provider': e.get('args', {}).get('provider')} for e in events if e.get('cat') == 'Node']
