"""Exercise the API through a real uhttpd CGI process when a target binary is available."""
import json
import os
from pathlib import Path
import shlex
import socket
import ssl
import subprocess
import time
import unittest
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from test_lite import BINARY, LiteCase

UHTTPD = os.environ.get('SIM_RELAY_TEST_UHTTPD')


@unittest.skipUnless(UHTTPD and Path(UHTTPD).exists(), 'Set SIM_RELAY_TEST_UHTTPD to test real HTTP')
class ApiHttpTests(LiteCase):
    def test_uhttpd_authentication_submission_and_status(self):
        token = '9' * 64
        self.assertNotIn('error', self.call('save_api_settings', enabled=1, scope='lan', port=8848,
                                           transport='http', token=token))
        revision = json.loads(self.config.read_text())['api']['revision']
        root = self.run_dir / f'api-{revision}'
        (root / 'cgi-bin').mkdir(parents=True)
        (root / 'allowed-subnets').write_text('127.0.0.0/8\n')
        executable = root / 'cgi-bin' / 'api'
        variables = ('SIM_RELAY_STATE', 'SIM_RELAY_RUNTIME', 'SIM_RELAY_CONFIG', 'SIM_RELAY_FIXTURE')
        exports = '\n'.join(f'export {name}={shlex.quote(self.env[name])}' for name in variables)
        executable.write_text(f'#!/bin/sh\n{exports}\nexec {shlex.quote(str(BINARY))} api\n')
        executable.chmod(0o755)
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
        server = subprocess.Popen([UHTTPD, '-f', '-h', str(root), '-x', '/cgi-bin', '-p', f'127.0.0.1:{port}', '-k', '0'],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.addCleanup(lambda: server.poll() is None and server.kill())

        def request(path, bearer, body=None):
            data = json.dumps(body).encode() if body is not None else None
            req = Request(f'http://127.0.0.1:{port}/cgi-bin/api' + path, data=data,
                          headers={'Authorization': f'Bearer {bearer}', 'Content-Type': 'application/json'})
            try:
                with urlopen(req, timeout=3) as response:
                    return response.status, json.load(response)
            except HTTPError as error:
                try: return error.code, json.load(error)
                finally: error.close()

        for _ in range(50):
            try:
                status, _ = request('/v1/devices', token); break
            except URLError:
                if server.poll() is not None: self.fail(server.stderr.read().decode())
                time.sleep(.05)
        else: self.fail('uhttpd did not start')
        self.assertEqual(status, 200)
        self.assertEqual(request('/v1/devices', '0' * 64)[0], 401)
        self.poll()
        body = {'device_id': self.device, 'request_id': 'real-http', 'number': '13800000000', 'text': '你好'}
        status, queued = request('/v1/messages', token, body)
        self.assertEqual((status, queued['status']), (202, 'queued'))
        self.poll()
        status, sent = request(f'/v1/messages?device_id={self.device}&request_id=real-http', token)
        self.assertEqual((status, sent['status']), (200, 'sent'))
        self.assertEqual(self.state()['posts'], 1)
        server.terminate(); server.communicate(timeout=5)

    @unittest.skipUnless(os.environ.get('SIM_RELAY_TEST_UHTTPD_TLS'), 'Target SSL provider not available')
    def test_uhttpd_https_uses_router_certificate(self):
        token = '8' * 64
        self.call('save_api_settings', enabled=1, scope='lan', port=8848, transport='https', token=token)
        revision = json.loads(self.config.read_text())['api']['revision']
        root = self.run_dir / f'api-{revision}'
        (root / 'cgi-bin').mkdir(parents=True)
        (root / 'allowed-subnets').write_text('127.0.0.0/8\n')
        executable = root / 'cgi-bin' / 'api'
        variables = ('SIM_RELAY_STATE', 'SIM_RELAY_RUNTIME', 'SIM_RELAY_CONFIG', 'SIM_RELAY_FIXTURE')
        exports = '\n'.join(f'export {name}={shlex.quote(self.env[name])}' for name in variables)
        executable.write_text(f'#!/bin/sh\n{exports}\nexec {shlex.quote(str(BINARY))} api\n')
        executable.chmod(0o755)
        cert, key = self.root / 'router.crt', self.root / 'router.key'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', '/CN=127.0.0.1', '-addext', 'subjectAltName=IP:127.0.0.1',
                        '-keyout', str(key), '-out', str(cert)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
        server = subprocess.Popen([UHTTPD, '-f', '-h', str(root), '-x', '/cgi-bin', '-s', f'127.0.0.1:{port}',
                                   '-C', str(cert), '-K', str(key)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.addCleanup(lambda: server.poll() is None and server.kill())
        req = Request(f'https://127.0.0.1:{port}/cgi-bin/api/v1/devices', headers={'Authorization': f'Bearer {token}'})
        context = ssl.create_default_context(cafile=str(cert))
        for _ in range(50):
            try:
                with urlopen(req, context=context, timeout=3) as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(json.load(response)['items'][0]['id'], self.device)
                break
            except URLError:
                if server.poll() is not None: self.fail(server.stderr.read().decode())
                time.sleep(.05)
        else: self.fail('HTTPS uhttpd did not start')
        server.terminate(); server.communicate(timeout=5)
