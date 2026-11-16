#!/usr/bin/env python3
"""Run the built x86-64/musl backend with packaged libraries and a temporary configuration and pending files.

No package install scripts, host services or real modem operations are executed.
"""
import argparse
import importlib.util
import io
import json
from pathlib import Path, PurePosixPath
import platform
import subprocess
import tarfile
import tempfile

spec = importlib.util.spec_from_file_location('ipk_inspection', Path(__file__).with_name('inspect-ipk.py'))
ipk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ipk)


def smoke(directory):
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'amd64'):
        raise ValueError('This smoke test requires an x86-64 Linux host')
    verification = ipk.verify(directory, machine=62)
    checks = []
    with tempfile.TemporaryDirectory(prefix='sim-relay-sdk-smoke-') as temporary:
        root = Path(temporary)
        libs = root / 'lib'
        libs.mkdir()
        aliases = {}
        binary = root / 'sim-relayd'
        for package in directory.glob('*.ipk'):
            fields, files = ipk.unpack_ipk(package)
            if fields['Package'] == 'sim-relay':
                binary.write_bytes(files['usr/sbin/sim-relayd'][0])
                binary.chmod(0o700)
            for name, (data, _) in files.items():
                path = PurePosixPath(name)
                if str(path.parent) not in ('lib', 'usr/lib'):
                    continue
                target = libs / path.name
                if target.exists() and target.read_bytes() != data:
                    raise ValueError(f'Conflicting packaged library: {name}')
                target.write_bytes(data)
                target.chmod(0o700)
            outer = ipk.ipk_members(package.read_bytes())
            raw = next(value[0] for name, value in outer.items() if name.startswith('data.tar'))
            with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
                for member in archive:
                    path = PurePosixPath(member.name.removeprefix('./'))
                    if member.issym() and str(path.parent) in ('lib', 'usr/lib'):
                        # Flatten aliases within the temporary library directory only.
                        target = PurePosixPath(member.linkname).name
                        if target in ('', '.', '..'):
                            raise ValueError('Invalid library alias')
                        if path.name in aliases and aliases[path.name] != target:
                            raise ValueError(f'Conflicting library alias: {path.name}')
                        aliases[path.name] = target
        for name, target in aliases.items():
            path = libs / name
            if not path.exists() and not path.is_symlink():
                path.symlink_to(target)
        loader = libs / 'ld-musl-x86_64.so.1'
        if not loader.exists():
            raise ValueError('Artifacts must include the matching libc IPK for the loader')
        prefix = [str(loader), '--library-path', str(libs), str(binary), '--state-dir', str(root / 'state')]

        def command(*args, body=None):
            result = subprocess.run(prefix + list(args), input=json.dumps(body or {}), text=True,
                                    capture_output=True, timeout=20, check=True,
                                    env={'PATH': '/usr/bin:/bin', 'LANG': 'C', 'HOME': str(root),
                                         'SIM_RELAY_CONFIG': str(root / 'config.json'),
                                         'SIM_RELAY_RUNTIME': str(root / 'runtime')})
            if result.stderr:
                raise ValueError(f'Target binary stderr: {result.stderr}')
            return json.loads(result.stdout) if result.stdout else None

        def call(method, **args):
            result = command('call', method, body=args)
            if result.get('error') and not result.get('id'):
                raise ValueError(f'{method}: {result["error"]}')
            return result

        status = call('status')
        checks.append('target musl loader and shared libraries load successfully')
        device = call('save_device', name='SDK smoke Mock', adapter='mock')['id']
        command('poll')
        messages = call('messages', device_id=device)['items']
        if len(messages) != 1 or '✓' not in messages[0]['text']:
            raise ValueError('Mock receive / Unicode check failed')
        checks.append('file configuration, live Mock receive and Unicode snapshot')
        request = dict(device_id=device, key='sdk-smoke-001', number='10086', text='SDK 编译验证')
        queued = call('send', **request)
        command('poll')
        sent = call('send', **request)
        if queued['id'] != sent['id'] or sent['status'] != 'sent':
            raise ValueError('Mock send / temporary idempotency check failed')
        checks.append('Mock send and temporary request lookup')
        cursor = root / 'state' / f'cursor-{device}.json'
        if not cursor.exists() or list((root / 'state').glob('*.db')):
            raise ValueError('Expected compact progress file without a database')
        checks.append('compact progress marker persisted without SQLite')
        (root / 'runtime' / 'state.json').unlink()
        command('poll')
        if len(call('messages', device_id=device)['items']) != 1:
            raise ValueError('Mock snapshot recovery failed')
        checks.append('snapshot recovery after volatile state loss')
        endpoint = dict(name='Duplicate check', adapter='zte_mf833u1', base_url='http://192.0.2.1', enabled=0)
        call('save_device', **endpoint)
        endpoint['base_url'] = 'http://192.0.2.1:80/'
        duplicate = command('call', 'save_device', body=endpoint)
        if 'already been added' not in duplicate.get('error', ''):
            raise ValueError('Equivalent ZTE device endpoint was not rejected')
        checks.append('duplicate endpoint rejected without contacting the device')
        return dict(passed=True, host=platform.machine(), version=status['version'],
                    elf=verification['elf'], checks=checks,
                    scope='Target binary with packaged musl libraries; Mock only, no chroot or services')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(smoke(args.directory), ensure_ascii=False, indent=2))
    except (ValueError, OSError, subprocess.SubprocessError, tarfile.TarError) as exc:
        parser.exit(1, f'SDK runtime smoke failed: {exc}\n')
