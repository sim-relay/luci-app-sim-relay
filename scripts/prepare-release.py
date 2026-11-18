#!/usr/bin/env python3
"""Stage the project IPKs for one SDK target with unique, target-prefixed names."""
import argparse
import importlib.util
from pathlib import Path
import shutil

spec = importlib.util.spec_from_file_location('inspect_ipk', Path(__file__).with_name('inspect-ipk.py'))
ipk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ipk)

TARGETS = {
    'x86-64': ('x86_64', 62, 64, 1),
    'aarch64_generic': ('aarch64_generic', 183, 64, 1),
    'aarch64_cortex-a53': ('aarch64_cortex-a53', 183, 64, 1),
    'aarch64_cortex-a72': ('aarch64_cortex-a72', 183, 64, 1),
    'arm_cortex-a7_neon-vfpv4': ('arm_cortex-a7_neon-vfpv4', 40, 32, 1),
    'arm_cortex-a9_vfpv3-d16': ('arm_cortex-a9_vfpv3-d16', 40, 32, 1),
    'arm_cortex-a15_neon-vfpv4': ('arm_cortex-a15_neon-vfpv4', 40, 32, 1),
    'mips_24kc': ('mips_24kc', 8, 32, 2),
    'mipsel_24kc': ('mipsel_24kc', 8, 32, 1),
}
PACKAGES = {
    'sim-relay': 'usr/sbin/sim-relayd',
    'sim-relay-modemmanager': 'usr/libexec/sim-relay-mm-pin',
    'luci-app-sim-relay': None,
    'luci-i18n-sim-relay-zh-cn': None,
}


def prepare(source, output, target):
    if target not in TARGETS:
        raise ValueError(f'Unsupported release target: {target}')
    architecture, machine, bits, endian = TARGETS[target]
    selected = []
    for package, binary in PACKAGES.items():
        matches = list(source.glob(f'{package}_*.ipk'))
        if len(matches) != 1:
            raise ValueError(f'Expected exactly one {package} IPK, found {len(matches)}')
        path = matches[0]
        fields, files = ipk.unpack_ipk(path)
        expected_arch = architecture if binary else 'all'
        if fields.get('Package') != package or fields.get('Architecture') != expected_arch:
            raise ValueError(f'{path.name}: expected {package} for {expected_arch}')
        if binary:
            if binary not in files:
                raise ValueError(f'{path.name}: missing {binary}')
            elf = files[binary][0]
            info = ipk.inspect_elf(elf)
            if info['machine'] != machine or info['bits'] != bits or elf[5] != endian:
                raise ValueError(f'{path.name}: ELF architecture does not match {target}')
        destination = output / f'{target}--{path.name}'
        if destination.exists():
            raise ValueError(f'Release asset already exists: {destination}')
        selected.append((path, destination))
    output.mkdir(parents=True, exist_ok=True)
    for source_path, destination in selected:
        shutil.copyfile(source_path, destination)
    return [path.name for _, path in selected]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', required=True, choices=TARGETS)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    for name in prepare(args.source, args.output, args.target):
        print(name)
