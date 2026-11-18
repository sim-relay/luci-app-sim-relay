#!/usr/bin/env python3
"""Inspect OpenWrt IPKs without extracting archive paths onto the filesystem."""
import argparse
import io
import json
from pathlib import Path, PurePosixPath
import struct
import tarfile
import re


def tar_members(data):
    result = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:*') as archive:
        for item in archive:
            name = item.name.removeprefix('./')
            if item.name.startswith('/') or '..' in PurePosixPath(name).parts:
                raise ValueError(f'Unsafe archive path: {item.name}')
            if item.isfile():
                if item.size > 64 * 1024 * 1024:
                    raise ValueError('Archive member exceeds inspection limit')
                result[name] = (archive.extractfile(item).read(), item.mode)
    return result


def ipk_members(data):
    if not data.startswith(b'!<arch>\n'):
        return tar_members(data)
    result, offset = {}, 8
    while offset < len(data):
        header = data[offset:offset + 60]
        if len(header) != 60 or header[58:] != b'`\n':
            raise ValueError('Invalid ar header')
        size = int(header[48:58])
        name = header[:16].decode().strip().rstrip('/')
        offset += 60
        if size < 0 or offset + size > len(data):
            raise ValueError('Invalid ar member size')
        result[name] = (data[offset:offset + size], 0)
        offset += size + size % 2
    return result


def unpack_ipk(path):
    outer = ipk_members(path.read_bytes())
    def part(prefix):
        names = [name for name in outer if name.startswith(prefix + '.tar')]
        if len(names) != 1:
            raise ValueError(f'{path.name}: missing or ambiguous {prefix} archive')
        return tar_members(outer[names[0]][0])
    control = part('control')['control'][0].decode()
    fields = {}
    for line in control.splitlines():
        if line and not line[0].isspace() and ':' in line:
            key, value = line.split(':', 1)
            fields[key] = value.strip()
    return fields, part('data')


def inspect_elf(data):
    if data[:4] != b'\x7fELF' or data[4] not in (1, 2) or data[5] not in (1, 2):
        raise ValueError('Service is not a supported ELF')
    bits = 64 if data[4] == 2 else 32
    order = '<' if data[5] == 1 else '>'
    def unpack(fmt, offset):
        return struct.unpack_from(order + fmt, data, offset)
    machine = unpack('H', 18)[0]
    if bits == 64:
        phoff = unpack('Q', 32)[0]
        phsize, phcount = unpack('HH', 54)
    else:
        phoff = unpack('I', 28)[0]
        phsize, phcount = unpack('HH', 42)
    loads, dynamic, interpreter = [], None, None
    for index in range(phcount):
        pos = phoff + index * phsize
        if bits == 64:
            kind, _, offset, address, _, size, _, _ = unpack('IIQQQQQQ', pos)
        else:
            kind, offset, address, _, size, _, _, _ = unpack('IIIIIIII', pos)
        if offset + size > len(data):
            raise ValueError('Invalid ELF segment size')
        if kind == 1:
            loads.append((address, size, offset))
        elif kind == 2:
            dynamic = (offset, size)
        elif kind == 3:
            interpreter = data[offset:offset + size].rstrip(b'\0').decode()
    needed, strings = [], None
    if dynamic:
        width, fmt = (16, 'QQ') if bits == 64 else (8, 'II')
        for pos in range(dynamic[0], sum(dynamic), width):
            tag, value = unpack(fmt, pos)
            if not tag:
                break
            if tag == 1:
                needed.append(value)
            elif tag == 5:
                strings = value
    libs = []
    if strings is not None:
        for address, size, offset in loads:
            if address <= strings < address + size:
                base = offset + strings - address
                for name in needed:
                    end = data.index(b'\0', base + name)
                    libs.append(data[base + name:end].decode())
                break
    return dict(bits=bits, machine=machine, interpreter=interpreter, needed=libs, bytes=len(data))


def verify_smtp_dependencies(fields):
    version = re.match(r'^(\d+)\.(\d+)\.(\d+)-r(\d+)(?:$|[^0-9])', fields.get('Version', ''))
    if version and tuple(map(int, version.groups())) >= (0, 6, 0, 8):
        dependencies = {part.strip().split(' ', 1)[0] for part in fields.get('Depends', '').split(',')}
        missing = {'msmtp', 'ca-bundle'} - dependencies
        if missing:
            raise ValueError('Missing SMTP dependencies: ' + ', '.join(sorted(missing)))


def verify(directory, machine=None, require_zh_cn=False):
    packages = {}
    for path in sorted(directory.glob('*.ipk')):
        fields, payload = unpack_ipk(path)
        name = fields.get('Package')
        if not name:
            raise ValueError(f'{path.name}: missing Package field')
        if name in packages:
            raise ValueError(f'Duplicate package: {name}')
        packages[name] = (path, fields, payload)
    for name in ('sim-relay', 'luci-app-sim-relay'):
        if name not in packages:
            raise ValueError(f'Missing compiled package: {name}')
    _, core, data = packages['sim-relay']
    verify_smtp_dependencies(core)
    required = ['usr/sbin/sim-relayd', 'etc/config/sim-relay', 'etc/init.d/sim-relay', 'usr/libexec/rpcd/sim-relay']
    for name in required:
        if name not in data:
            raise ValueError(f'Missing backend file: {name}')
    for name in (required[0], required[2], required[3]):
        if not data[name][1] & 0o111:
            raise ValueError(f'Installed file is not executable: {name}')
    match = re.match(r'0\.6\.0-r(\d+)', core.get('Version', ''))
    if match and int(match.group(1)) >= 10:
        for name in ('etc/init.d/sim-relay-api', 'usr/libexec/sim-relay-api'):
            if name not in data or not data[name][1] & 0o111:
                raise ValueError(f'Missing executable API component: {name}')
    binary = data[required[0]][0]
    elf = inspect_elf(binary)
    if machine is not None and elf['machine'] != machine:
        raise ValueError(f'ELF machine {elf["machine"]} != requested {machine}')
    if not (elf['interpreter'] or '').startswith('/lib/ld-musl-'):
        raise ValueError('Backend does not use the OpenWrt musl interpreter')
    version = re.match(r'(\d+)\.(\d+)', core.get('Version', ''))
    management = version is not None and tuple(map(int, version.groups())) >= (0, 3)
    lightweight = version is not None and tuple(map(int, version.groups())) >= (0, 4)
    required_libraries = ['libcurl.so', 'libcjson.so']
    if not lightweight:
        required_libraries.append('libsqlite3.so')
    elif any('sqlite' in name.lower() for name in elf['needed']) or 'sqlite' in core.get('Depends', '').lower():
        raise ValueError('Lightweight backend unexpectedly depends on SQLite')
    if management:
        required_libraries.append('libsodium.so')
    for lib in required_libraries:
        if not any(name.startswith(lib) for name in elf['needed']):
            raise ValueError(f'Missing shared library dependency: {lib}')
    if b'SIM_RELAY_FIXTURE' in binary:
        raise ValueError('Test transport was linked into the installed backend')
    if version is not None and tuple(map(int, version.groups())) >= (0, 5):
        keep = data.get('lib/upgrade/keep.d/sim-relay', (b'', 0))[0].decode().splitlines()
        expected = {'/etc/config/sim-relay', '/etc/sim-relay/credentials.key', '/etc/sim-relay/cursor-*.json', '/etc/sim-relay/pending/'}
        if not expected <= set(keep):
            raise ValueError('Missing sysupgrade state preservation rules')
        for _, fields, _ in packages.values():
            if fields.get('Package') in ('sim-relay', 'luci-app-sim-relay', 'sim-relay-modemmanager'):
                if re.search(r'(^|[,\s])modemmanager(?=[,\s(]|$)', fields.get('Depends', '')):
                    raise ValueError('ModemManager must remain a runtime-only optional requirement')
        if 'sim-relay-modemmanager' in packages:
            _, _, helper = packages['sim-relay-modemmanager']
            name = 'usr/libexec/sim-relay-mm-pin'
            if name not in helper or not helper[name][1] & 0o111:
                raise ValueError('Missing executable optional PIN helper')
            info = inspect_elf(helper[name][0])
            if info['machine'] != elf['machine'] or not any(v.startswith('libdbus-1.so') for v in info['needed']):
                raise ValueError('Invalid PIN helper architecture or D-Bus linkage')
    _, luci, data = packages['luci-app-sim-relay']
    menu = 'usr/share/luci/menu.d/luci-app-sim-relay.json'
    acl = 'usr/share/rpcd/acl.d/luci-app-sim-relay.json'
    view = 'www/luci-static/resources/view/sim-relay/overview.js'
    for name in (menu, acl, view):
        if name not in data:
            raise ValueError(f'Missing LuCI file: {name}')
    if management:
        for name in (json.loads(data[menu][0])['admin/services/sim-relay/' + page]['action']['path'].rsplit('/', 1)[-1] + '.js' for page in ('manage', 'channels')):
            if 'www/luci-static/resources/view/sim-relay/' + name not in data:
                raise ValueError(f'Missing management view: {name}')
    english_menu = json.loads(data[menu][0])
    if match and int(match.group(1)) >= 10:
        api_view = english_menu.get('admin/services/sim-relay/api', {}).get('action', {}).get('path', '')
        if not api_view or 'www/luci-static/resources/view/' + api_view + '.js' not in data:
            raise ValueError('Missing API configuration view')
    if version is not None and tuple(map(int, version.groups())) >= (0, 6):
        for node in english_menu.values():
            if node.get('action', {}).get('type') == 'view' and 'luci-app-sim-relay' not in node.get('depends', {}).get('acl', []):
                raise ValueError('LuCI view missing its own ACL declaration')
    # English files stay in the app; luci.mk generates .lmo files in a separate IPK.
    if require_zh_cn:
        if 'luci-i18n-sim-relay-zh-cn' not in packages:
            raise ValueError('Missing requested Simplified Chinese language package')
    chinese = packages.get('luci-i18n-sim-relay-zh-cn')
    if chinese:
        _, fields, translated = chinese
        lmo = 'usr/lib/lua/luci/i18n/sim-relay.zh-cn.lmo'
        defaults = 'etc/uci-defaults/luci-i18n-sim-relay-zh-cn'
        if lmo not in translated or not translated[lmo][0] or defaults not in translated:
            raise ValueError('Incomplete Simplified Chinese language package')
        if fields.get('Architecture') != 'all' or 'luci-app-sim-relay' not in fields.get('Depends', ''):
            raise ValueError('Invalid Chinese language package architecture/dependency')
        if any(name.endswith('.js') or name.endswith('.po') for name in translated):
            raise ValueError('Language package must contain compiled translations, not app sources')
    if require_zh_cn or chinese or (version is not None and tuple(map(int, version.groups())) >= (0, 6)):
        if any(name.endswith(('.lmo', '.po', '.pot')) for name in data):
            raise ValueError('Translations must not be bundled into the English application')
        if re.search(r'[\u3400-\u9fff]', json.dumps(english_menu, ensure_ascii=False)):
            raise ValueError('Non-English application menu')
        for name, (raw, _) in data.items():
            if name.endswith('.js') and re.search(r'[\u3400-\u9fff]', raw.decode()):
                raise ValueError('Chinese text bundled into the English application')
        if 'luci-i18n-' in luci.get('Depends', ''):
            raise ValueError('English application must not require a language package')
    grants = json.loads(data[acl][0])['luci-app-sim-relay']
    if not ({'send'} if lightweight else {'send', 'acknowledge_send'}) <= set(grants['write']['ubus']['sim-relay']):
        raise ValueError('Missing sending ACL')
    if 'send_history' not in grants['read']['ubus']['sim-relay']:
        raise ValueError('Missing send-history ACL')
    if management and not {'save_pin', 'unlock_pin', 'delete_messages', 'save_channel', 'bind_channels'} <= set(grants['write']['ubus']['sim-relay']):
        raise ValueError('Missing management/channel ACL')
    return dict(verified=True, elf=elf, packages=[dict(
        name=name, file=path.name, bytes=path.stat().st_size,
        installed_payload_bytes=sum(len(value[0]) for value in payload.values()),
        version=fields.get('Version'), architecture=fields.get('Architecture'),
        depends=fields.get('Depends', '')
    ) for name, (path, fields, payload) in packages.items()])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--machine', type=int, help='Expected ELF e_machine, e.g. 62 for x86-64')
    parser.add_argument('--require-zh-cn', action='store_true', help='Require the separate Chinese translation IPK')
    args = parser.parse_args()
    try:
        print(json.dumps(verify(args.directory, args.machine, args.require_zh_cn), indent=2))
    except (ValueError, KeyError, tarfile.TarError, UnicodeError, struct.error) as exc:
        parser.exit(1, f'Package verification failed: {exc}\n')
