"""Generate synthetic, non-sensitive, fixed OCR inputs (requires existing Pillow)."""
import hashlib
import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
LINES = {
    'sparse': ['EDGE DOCUMENT TEST', 'Sample number 1042.'],
    'medium': ['EDGE DOCUMENT TEST', 'The camera records one printed page.',
               'The local model reads the text.', 'Task number 1042 is complete.'],
    'dense': ['EDGE DOCUMENT TEST', 'A small system captures a printed page.',
              'The camera records an image in memory.', 'The worker reads one task at a time.',
              'A full queue returns a busy message.', 'Each result has a task number.',
              'The service uses the local CPU only.', 'The final output is checked for errors.'],
}

def main():
    dest = ROOT / 'fixtures'
    dest.mkdir(exist_ok=True)
    font = ImageFont.load_default(size=32)
    manifest = []
    for name, lines in LINES.items():
        image = Image.new('RGB', (1000, 520), 'white')
        draw = ImageDraw.Draw(image)
        for index, line in enumerate(lines):
            draw.text((45, 35 + index * 53), line, fill='black', font=font)
        path = dest / (name + '.png')
        image.save(path, format='PNG')
        reference = '\n'.join(lines) + '\n'
        (dest / (name + '.txt')).write_text(reference, encoding='utf-8')
        manifest.append({'id': name, 'image': path.name, 'reference': name+'.txt',
                         'image_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                         'width': 1000, 'height': 520, 'pixel_format': 'RGB'})
    (dest / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')

if __name__ == '__main__':
    main()
