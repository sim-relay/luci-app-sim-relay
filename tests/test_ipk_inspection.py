"""Check the artifact validator with synthetic IPKs, without Docker or an SDK."""
import importlib.util
import io
from pathlib import Path
import struct
import re
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('inspect_ipk', Path(__file__).resolve().parents[1] / 'scripts/inspect-ipk.py')
ipk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ipk)


def archive(files):
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode='w:gz') as tar:
        for name, (data, mode) in files.items():
            entry = tarfile.TarInfo('./' + name)
            entry.size, entry.mode = len(data), mode
            tar.addfile(entry, io.BytesIO(data))
    return buffer.getvalue()


class ArtifactTests(unittest.TestCase):
    def test_smtp_dependencies_required_from_release_eight(self):
        ipk.verify_smtp_dependencies({'Version': '0.6.0-r7', 'Depends': 'libc'})
        ipk.verify_smtp_dependencies({'Version': '0.6.0-r8', 'Depends': 'libc, msmtp, ca-bundle'})
        with self.assertRaisesRegex(ValueError, 'ca-bundle'):
            ipk.verify_smtp_dependencies({'Version': '0.6.0-r8', 'Depends': 'msmtp'})
        with self.assertRaisesRegex(ValueError, 'msmtp'):
            ipk.verify_smtp_dependencies({'Version': '0.7.0-r1', 'Depends': 'ca-bundle'})

    def test_both_packages_are_visible_to_openwrt_scanner(self):
        root = Path(__file__).resolve().parents[1]
        # include/scan.mk greps the package Makefile itself, not included luci.mk.
        marker = re.compile(r'call (Build/DefaultTargets|BuildPackage|KernelPackage)')
        for name in ('package/sim-relay/Makefile', 'luci-app-sim-relay/Makefile'):
            self.assertRegex((root / name).read_text(), marker)

    def test_outer_tar_and_ar_formats(self):
        control = archive({'control': (b'Package: sim-relay\nArchitecture: x86_64\n', 0o644)})
        payload = archive({'usr/sbin/example': (b'payload', 0o755)})
        parts = {'control.tar.gz': (control, 0o644), 'data.tar.gz': (payload, 0o644)}
        ar = bytearray(b'!<arch>\n')
        for name, (data, _) in parts.items():
            header = f'{name + "/":<16}{0:<12}{0:<6}{0:<6}{100644:<8}{len(data):<10}`\n'.encode()
            self.assertEqual(len(header), 60)
            ar.extend(header + data + (b'\n' if len(data) % 2 else b''))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.ipk'
            for data in (archive(parts), bytes(ar)):
                path.write_bytes(data)
                fields, files = ipk.unpack_ipk(path)
                self.assertEqual(fields['Package'], 'sim-relay')
                self.assertEqual(files['usr/sbin/example'], (b'payload', 0o755))

    def test_elf_machine_and_interpreter(self):
        data = bytearray(512)
        data[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<H', data, 18, 62)
        struct.pack_into('<Q', data, 32, 64)
        struct.pack_into('<HH', data, 54, 56, 1)
        interpreter = b'/lib/ld-musl-x86_64.so.1\0'
        struct.pack_into('<IIQQQQQQ', data, 64, 3, 0, 256, 0, 0, len(interpreter), len(interpreter), 1)
        data[256:256 + len(interpreter)] = interpreter
        parsed = ipk.inspect_elf(bytes(data))
        self.assertEqual(parsed['machine'], 62)
        self.assertEqual(parsed['bits'], 64)
        self.assertEqual(parsed['interpreter'], interpreter[:-1].decode())

    def test_invalid_and_missing_artifacts_rejected(self):
        with self.assertRaises(ValueError):
            ipk.inspect_elf(b'not an ELF')
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, 'Missing compiled package'):
                ipk.verify(Path(directory))
        with self.assertRaisesRegex(ValueError, 'Unsafe archive path'):
            ipk.tar_members(archive({'../outside': (b'never extracted', 0o644)}))

    def test_host_glibc_binary_is_not_mistaken_for_openwrt(self):
        binary = Path(__file__).resolve().parents[1] / 'build/sim-relayd'
        if not binary.exists():
            self.skipTest('Native binary is not built')
        elf = ipk.inspect_elf(binary.read_bytes())
        self.assertFalse(any(name.startswith('libsqlite3.so') for name in elf['needed']))
        self.assertFalse((elf['interpreter'] or '').startswith('/lib/ld-musl-'))


if __name__ == '__main__':
    unittest.main()
