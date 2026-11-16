#!/usr/bin/python3
"""Offline executable substitute for mmcli and the private PIN helper."""
import json
import os
from pathlib import Path
import sys

file = Path(os.environ['SIM_RELAY_MM_FIXTURE'])
s = json.loads(file.read_text())
a = sys.argv[1:]
s.setdefault('calls', []).append(a)
modem = '/org/freedesktop/ModemManager1/Modem/' + str(s.get('index', 0))
sim = '/org/freedesktop/ModemManager1/SIM/0'
out = {}
code = 0

def modem_info(equipment=None):
    return {'modem': {'dbus-path': modem, 'generic': {
        'model': 'Test LTE', 'manufacturer': 'Fixture', 'equipment-identifier': equipment or s.get('equipment', '123456789012345'),
        'sim': sim, 'state': 'locked' if s.get('locked') else s.get('unlocked_state', 'registered'),
        'unlock-required': s.get('reported_lock', 'sim-pin' if s.get('locked') else s.get('unlocked_lock', 'none')),
        'unlock-retries': ['sim-pin2 (1)', f"sim-pin ({s.get('tries', 3)})"]}, '3gpp': {'operator-name': 'Test'}}}

if a == [sim]:
    pin = sys.stdin.read()
    s['pin_calls'] = s.get('pin_calls', 0) + 1
    if pin == s.get('expected_pin', '1234'):
        s['locked'] = False
    else:
        s['tries'] = s.get('tries', 3) - 1
        code = 1
elif a[0] != '--output-json':
    code = 1
elif '-L' in a:
    out = {'modem-list': [modem] if not s.get('absent') else []}
    if s.get('second'):
        out['modem-list'].append('/org/freedesktop/ModemManager1/Modem/99')
elif '-i' in a:
    out = {'sim': {'properties': {'iccid': s.get('iccid', '8986001234567890123')}}}
elif '-s' in a:
    path = a[a.index('-s')+1]
    sms = next((m for m in s.get('messages', []) if m['path'] == path), None)
    if not sms:
        code = 1
    else:
        if '--send' in a:
            sms['state'] = 'sent'
            s['sends'] = s.get('sends', 0) + 1
            if s.get('send_failure'):
                code = 1
        out = {'sms': {'content': {'number': sms['number'], 'text': sms['text']},
                       'properties': {'state': sms['state'], 'timestamp': sms.get('timestamp', '--')}}}
elif '-m' in a:
    path = a[a.index('-m')+1]
    if '--enable' in a:
        s['enable_calls'] = s.get('enable_calls', 0) + 1
        if s.get('enable_fails'): code = 1
        else: s['unlocked_state'] = 'enabled'
    elif '--messaging-list-sms' in a:
        out = {'modem.messaging.sms': [m['path'] for m in s.get('messages', [])]}
    elif any(v.startswith('--messaging-create-sms=') for v in a):
        number = next(v.split('number=', 1)[1] for v in a if v.startswith('--messaging-create-sms='))
        textfile = Path(next(v.split('=', 1)[1] for v in a if v.startswith('--messaging-create-sms-with-text=')))
        target = f"/org/freedesktop/ModemManager1/SMS/{100+s.get('sends', 0)}"
        s.setdefault('messages', []).append(dict(path=target, number=number, text=textfile.read_text(), state='stored'))
        s['text_file'] = str(textfile)
        s['text_mode'] = textfile.stat().st_mode & 0o777
        out = {'modem': {'messaging': {'created-sms': target}}}
    elif any(v.startswith('--messaging-delete-sms=') for v in a):
        target = next(v.split('=', 1)[1] for v in a if v.startswith('--messaging-delete-sms='))
        s['messages'] = [m for m in s.get('messages', []) if m['path'] != target]
        s['deletes'] = s.get('deletes', 0) + 1
    else:
        out = modem_info('999999999999999' if path.endswith('/99') else None)
else:
    code = 1
file.write_text(json.dumps(s))
print(json.dumps(out))
sys.exit(code)
