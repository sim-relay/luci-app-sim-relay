"""Release staging must keep architecture identities and reject incomplete builds."""
import importlib.util
import io
from pathlib import Path
import struct
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('prepare_release', Path(__file__).resolve().parents[1] / 'scripts/prepare-release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


def archive(files):
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode='w:gz') as tar:
        for name, data in files.items():
            item = tarfile.TarInfo('./' + name); item.size = len(data)
            tar.addfile(item, io.BytesIO(data))
    return out.getvalue()


def elf(machine, bits, endian):
    result = bytearray(64)
    result[:6] = bytes([0x7f, ord('E'), ord('L'), ord('F'), 2 if bits == 64 else 1, endian])
    order = '<' if endian == 1 else '>'
    struct.pack_into(order + 'H', result, 18, machine)
    struct.pack_into(order + ('Q' if bits == 64 else 'I'), result, 32 if bits == 64 else 28, 64)
    struct.pack_into(order + 'HH', result, 54 if bits == 64 else 42, 56 if bits == 64 else 32, 0)
    return bytes(result)


def package(directory, name, arch, executable=None, binary=None):
    control = archive({'control': f'Package: {name}\nVersion: 1-r1\nArchitecture: {arch}\n'.encode()})
    data = archive({executable: binary} if executable else {})
    path = directory / f'{name}_1-r1_{arch}.ipk'
    path.write_bytes(archive({'control.tar.gz': control, 'data.tar.gz': data}))
    return path


class ReleaseAssetsTests(unittest.TestCase):
    def make_packages(self, root, target):
        architecture, machine, bits, endian = release.TARGETS[target]
        for name, binary in release.PACKAGES.items():
            package(root, name, architecture if binary else 'all', binary,
                    elf(machine, bits, endian) if binary else None)

    def test_all_packages_are_target_prefixed_and_no_collision(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); first = root / 'first'; second = root / 'second'; output = root / 'assets'
            first.mkdir(); second.mkdir(); self.make_packages(first, 'x86-64'); self.make_packages(second, 'aarch64_generic')
            a = release.prepare(first, output, 'x86-64'); b = release.prepare(second, output, 'aarch64_generic')
            self.assertEqual(len(a), 4); self.assertEqual(len(b), 4)
            self.assertTrue(all(name.startswith('x86-64--') for name in a))
            self.assertTrue(all(name.startswith('aarch64_generic--') for name in b))
            self.assertEqual(len(list(output.glob('*.ipk'))), 8)
            with self.assertRaisesRegex(ValueError, 'already exists'):
                release.prepare(first, output, 'x86-64')

    def test_missing_or_wrong_architecture_fails_without_partial_assets(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); source = root / 'source'; source.mkdir(); output = root / 'assets'
            self.make_packages(source, 'mipsel_24kc')
            with self.assertRaisesRegex(ValueError, 'expected sim-relay for mips_24kc'):
                release.prepare(source, output, 'mips_24kc')
            self.assertFalse(output.exists())
            (source / 'sim-relay-modemmanager_1-r1_mipsel_24kc.ipk').unlink()
            with self.assertRaisesRegex(ValueError, 'exactly one sim-relay-modemmanager'):
                release.prepare(source, output, 'mipsel_24kc')
            self.assertFalse(output.exists())

    def test_mips_endianness_is_verified(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'source'; source.mkdir(); self.make_packages(source, 'mipsel_24kc')
            package(source, 'sim-relay', 'mipsel_24kc', 'usr/sbin/sim-relayd', elf(8, 32, 2))
            with self.assertRaisesRegex(ValueError, 'ELF architecture'):
                release.prepare(source, Path(tmp) / 'out', 'mipsel_24kc')


if __name__ == '__main__':
    unittest.main()
