#!/usr/bin/env python3
"""Check English LuCI source and separately packaged Simplified Chinese translations."""
import ast
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
HAN = re.compile(r'[\u3400-\u9fff]')
LITERAL = re.compile(r'''_\(\s*('(?:\\.|[^'\\])*'|"(?:\\.|[^"\\])*")\s*\)''')


def read_po(path):
    entries = {}
    msgid = msgstr = None
    part = None
    fuzzy = False

    def finish():
        if msgid is None:
            return
        if msgid in entries:
            raise ValueError(f'{path}: duplicate msgid {msgid!r}')
        if msgstr is None or fuzzy:
            raise ValueError(f'{path}: incomplete or fuzzy entry {msgid!r}')
        entries[msgid] = msgstr

    for line in path.read_text().splitlines() + ['']:
        line = line.strip()
        if not line:
            finish()
            msgid = msgstr = part = None
            fuzzy = False
        elif line.startswith('#,') and 'fuzzy' in line:
            fuzzy = True
        elif line.startswith('#'):
            continue
        elif line.startswith('msgid '):
            if msgid is not None:
                raise ValueError(f'{path}: missing entry separator')
            msgid, part = ast.literal_eval(line[6:]), 'id'
        elif line.startswith('msgstr '):
            msgstr, part = ast.literal_eval(line[7:]), 'str'
        elif line.startswith('"') and part:
            if part == 'id':
                msgid += ast.literal_eval(line)
            else:
                msgstr += ast.literal_eval(line)
        else:
            raise ValueError(f'{path}: unsupported PO syntax: {line}')
    return entries


def check(root=ROOT):
    app = root / 'luci-app-sim-relay'
    keys = set()
    for path in (app / 'htdocs').rglob('*.js'):
        source = path.read_text()
        if HAN.search(source):
            raise ValueError(f'Chinese text in English application: {path}')
        keys.update(ast.literal_eval(m[1]) for m in LITERAL.finditer(source))
    menu = json.loads((app / 'root/usr/share/luci/menu.d/luci-app-sim-relay.json').read_text())
    keys.update(entry['title'] for entry in menu.values())
    if any(HAN.search(key) for key in keys):
        raise ValueError('Translation message IDs must be English')
    template = read_po(app / 'po/templates/sim-relay.pot')
    chinese = read_po(app / 'po/zh_Hans/sim-relay.po')
    if set(template) != set(chinese):
        raise ValueError('Template and Chinese catalog message IDs differ')
    if keys - template.keys():
        raise ValueError(f'Missing source strings: {sorted(keys - template.keys())}')
    for key, value in chinese.items():
        if key and (not value or HAN.search(key)):
            raise ValueError(f'Untranslated or non-English message ID: {key!r}')
    if 'Language: zh_Hans\n' not in chinese.get('', ''):
        raise ValueError('Chinese catalog has no zh_Hans header')
    return dict(verified=True, source_strings=len(keys), chinese_entries=len(chinese)-1,
                package='luci-i18n-sim-relay-zh-cn')


if __name__ == '__main__':
    print(json.dumps(check(), ensure_ascii=False, indent=2))
