"""Exercise the OpenWrt init script with isolated netifd/procd fixtures."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.geteuid() == 0, 'The init script requires a root-owned runtime directory')
class ApiServiceTests(unittest.TestCase):
    def test_lan_and_all_network_bindings(self):
        with tempfile.TemporaryDirectory() as temporary:
            tmp = Path(temporary); (tmp / 'bin').mkdir()
            runtime = tmp / 'runtime'; cgi = tmp / 'cgi'; cgi.write_text('#!/bin/sh\nexit 0\n'); cgi.chmod(0o755)
            uci = tmp / 'bin/uci'
            uci.write_text('#!/bin/sh\n[ "$3" = sim-relay.main.data ] && printf "%s" "$SIM_RELAY_MOCK_CONFIG"\n')
            uci.chmod(0o755)
            jsonfilter = tmp / 'bin/jsonfilter'
            jsonfilter.write_text('#!/usr/bin/python3\nimport json,sys\nvalue=json.load(sys.stdin)\n'
                                  'for part in sys.argv[-1].removeprefix("@.").split("."): value=value[part]\n'
                                  'print(value)\n')
            jsonfilter.chmod(0o755)
            network = tmp / 'network.sh'
            network.write_text("network_get_subnets() { eval \"$1='192.168.50.1/24'\"; }\n"
                               "network_get_subnets6() { eval \"$1='fd50::1/64'\"; }\n"
                               "network_get_ipaddrs() { eval \"$1='192.168.50.1'\"; }\n"
                               "network_get_ipaddrs6() { eval \"$1='fd50::1'\"; }\n")
            source = (ROOT / 'package/sim-relay/files/sim-relay-api.init').read_text()
            source = source.replace('/tmp/sim-relay', str(runtime)).replace('/usr/libexec/sim-relay-api', str(cgi))
            source = source.replace('/lib/functions/network.sh', str(network))
            script = tmp / 'init'; script.write_text(source)
            runner = tmp / 'run'
            runner.write_text('#!/bin/sh\nprocd_open_instance() { :; }\nprocd_close_instance() { :; }\n'
                              'procd_set_param() { printf "%s\\n" "$@" >> "$CAPTURE"; }\n'
                              'procd_append_param() { printf "%s\\n" "$@" >> "$CAPTURE"; }\n'
                              '. "$SCRIPT"\nstart_service\n')
            runner.chmod(0o755)
            capture = tmp / 'capture'
            env = dict(os.environ, PATH=f"{tmp / 'bin'}:{os.environ['PATH']}", CAPTURE=str(capture), SCRIPT=str(script))

            def run(scope, revision):
                capture.write_text('')
                env['SIM_RELAY_MOCK_CONFIG'] = json.dumps({'api': {'enabled': 1, 'port': 8848, 'scope': scope,
                                                                    'transport': 'http', 'revision': revision}})
                subprocess.run([str(runner)], env=env, check=True, capture_output=True, text=True)
                return capture.read_text().splitlines()

            lan = run('lan', 1)
            self.assertIn('127.0.0.1:8848', lan); self.assertIn('192.168.50.1:8848', lan)
            self.assertIn('[fd50::1]:8848', lan); self.assertNotIn('0.0.0.0:8848', lan)
            allowed = (runtime / 'api-1/allowed-subnets').read_text().splitlines()
            self.assertIn('192.168.50.1/24', allowed); self.assertIn('fd50::1/64', allowed)
            self.assertTrue((runtime / 'api-1/cgi-bin/api').stat().st_mode & 0o111)
            anywhere = run('any', 2)
            self.assertIn('0.0.0.0:8848', anywhere); self.assertIn('[::]:8848', anywhere)
            target = tmp / 'unrelated'; target.mkdir()
            (runtime / 'api-3').symlink_to(target, target_is_directory=True)
            env['SIM_RELAY_MOCK_CONFIG'] = json.dumps({'api': {'enabled': 1, 'port': 8848, 'scope': 'any',
                                                                'transport': 'http', 'revision': 3}})
            self.assertNotEqual(subprocess.run([str(runner)], env=env, capture_output=True).returncode, 0)
            self.assertFalse((target / 'cgi-bin').exists())
