"""Offline regression tests for UCI/file architecture and restart progress recovery."""
import json
import os
from pathlib import Path
import subprocess
import tarfile
import glob
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get('SIM_RELAY_FIXTURE_BINARY', ROOT / 'build/sim-relayd-fixture')).resolve()

@unittest.skipUnless(BINARY.exists(), 'Build offline fixture binary first')
class LiteCase(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.state_dir = self.root / 'persistent'
        self.run_dir = self.root / 'runtime'
        self.config = self.root / 'config.json'
        self.fixture = self.root / 'modem.json'
        self.fixture.write_text(json.dumps(dict(mode='normal', messages=[], posts=0)))
        self.env = dict(os.environ, SIM_RELAY_STATE=str(self.state_dir), SIM_RELAY_RUNTIME=str(self.run_dir),
                        SIM_RELAY_CONFIG=str(self.config), SIM_RELAY_FIXTURE=str(self.fixture))
        self.device = self.call('save_device', name='SIM 测试', adapter='zte_mf833u1', base_url='http://192.0.2.1')['id']

    def run_binary(self, *args, data=None, check=True):
        return subprocess.run([str(BINARY), *args], input=json.dumps(data or {}), text=True,
                              capture_output=True, env=self.env, check=check, timeout=15)

    def call(self, method, **args):
        return json.loads(self.run_binary('call', method, data=args).stdout)

    def state(self):
        return json.loads(self.fixture.read_text())

    def change(self, **args):
        v = self.state(); v.update(args); self.fixture.write_text(json.dumps(v))

    def message(self, slot, second, content='新短信', hour=12, minute=0):
        return dict(id=slot, tag='1', number='10086', content=content.encode('utf-16-be').hex().upper(),
                    date=f'26,11,14,{hour:02},{minute:02},{second:02}')

    def poll(self, reset=False):
        if reset:
            (self.run_dir / 'state.json').unlink(missing_ok=True)
        self.call('sync', device_id=self.device)
        self.run_binary('poll')

    def channel(self, kind='wecom', **kwargs):
        config, secrets = ({}, {'webhook_url': 'https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=abcdefgh1234'})
        if kind == 'sms': config, secrets = {'number': '13800000000'}, {}
        v = self.call('save_channel', id=0, name='转发', kind=kind, enabled=1, version=0,
                      config=json.dumps(config), secrets=json.dumps(secrets), **kwargs)
        self.assertNotIn('error', v)
        cid = v['id']; self.call('bind_channels', device_id=self.device, ids=json.dumps([cid])); return cid

    def pending(self):
        return list((self.state_dir / 'pending').glob('*.json'))

class LiteTests(LiteCase):
    def api_request(self, method, path, token='', body=None, query='', remote='192.168.1.23', secure=True, revision=None):
        data = json.dumps(body or {}) if body is not None else ''
        api = json.loads(self.config.read_text()).get('api', {})
        docroot = self.run_dir / f"api-{api.get('revision', 0) if revision is None else revision}"
        docroot.mkdir(exist_ok=True)
        (docroot / 'allowed-subnets').write_text('127.0.0.0/8\n192.168.1.1/24\nfd01::1/64\n')
        env = dict(self.env, REQUEST_METHOD=method, PATH_INFO=path, QUERY_STRING=query,
                   HTTP_AUTHORIZATION=('Bearer ' + token if token else ''),
                   CONTENT_TYPE='application/json', CONTENT_LENGTH=str(len(data.encode())), HTTPS='on' if secure else '',
                   REMOTE_ADDR=remote, DOCUMENT_ROOT=str(docroot))
        result = subprocess.run([str(BINARY), 'api'], input=data, text=True, capture_output=True,
                                env=env, check=True, timeout=15)
        header, result_body = result.stdout.split('\n\n', 1)
        return int(header.split()[1]), json.loads(result_body)

    def test_api_settings_hide_token_and_reject_invalid_values(self):
        token = 'a' * 64
        self.assertIn('error', self.call('save_api_settings', enabled=1, scope='lan', port=8848, token=''))
        self.assertIn('error', self.call('save_api_settings', enabled=1, scope='lan', port=8848, token='short'))
        self.assertNotIn('error', self.call('save_api_settings', enabled=1, scope='lan', port=8848, token=token))
        self.assertNotIn(token, self.config.read_text())
        self.assertNotIn(token, json.dumps(self.call('api_settings')))
        self.assertTrue(self.call('api_settings')['has_token'])

    def test_api_authentication_send_and_status(self):
        token = 'b' * 64
        self.assertEqual(self.api_request('GET', '/v1/devices', token)[0], 404)
        self.call('save_api_settings', enabled=1, scope='lan', port=8848, token=token)
        self.assertEqual(self.api_request('GET', '/v1/devices', 'c' * 64)[0], 401)
        self.assertEqual(self.api_request('GET', '/v1/devices', token, secure=False)[0], 403)
        self.assertEqual(self.api_request('GET', '/v1/devices', token, revision=99)[0], 503)
        self.assertEqual(self.api_request('GET', '/v1/devices', token, remote='203.0.113.1')[0], 403)
        self.assertEqual(self.api_request('GET', '/v1/devices', token, remote='fd01::123')[0], 200)
        status, devices = self.api_request('GET', '/v1/devices', token)
        self.assertEqual(status, 200); self.assertEqual(devices['items'][0]['id'], self.device)
        self.poll()
        request = dict(device_id=self.device, request_id='test-1', number='13800000000', text='hello')
        status, queued = self.api_request('POST', '/v1/messages', token, request)
        self.assertEqual(status, 202); self.assertEqual(queued['status'], 'queued')
        self.assertNotIn('number', queued); self.assertNotIn('text', queued)
        self.assertEqual(self.api_request('POST', '/v1/messages', token, request)[0], 202)
        status, queued = self.api_request('GET', '/v1/messages', token, query=f'device_id={self.device}&request_id=test-1')
        self.assertEqual(status, 200); self.assertEqual(queued['status'], 'queued')
        self.poll()
        status, sent = self.api_request('GET', '/v1/messages', token, query=f'request_id=test-1&device_id={self.device}')
        self.assertEqual(status, 200); self.assertEqual(sent['status'], 'sent'); self.assertEqual(self.state()['posts'], 1)
        self.assertEqual(self.api_request('POST', '/v1/messages', token, {**request, 'text': 'different'})[0], 400)
        self.assertEqual(self.api_request('GET', '/v1/messages', token, query='device_id=1&request_id=missing')[0], 404)
        self.assertEqual(self.api_request('POST', '/v1/messages', token, {**request, 'request_id': '../unsafe'})[0], 400)

    def test_api_rate_limit(self):
        token = 'd' * 64
        self.call('save_api_settings', enabled=1, scope='any', port=8848, token=token)
        self.assertEqual(self.api_request('GET', '/v1/devices', token, remote='203.0.113.1')[0], 200)
        request = dict(device_id=self.device, request_id='same-key', number='13800000000', text='hello')
        for _ in range(30): self.assertEqual(self.api_request('POST', '/v1/messages', token, request)[0], 202)
        self.assertEqual(self.api_request('POST', '/v1/messages', token, request)[0], 429)

    def test_api_retry_returns_existing_failed_result(self):
        token = 'f' * 64
        self.call('save_api_settings', enabled=1, scope='lan', port=8848, token=token)
        self.poll()
        request = dict(device_id=self.device, request_id='failed-once', number='13800000000', text='hello')
        self.assertEqual(self.api_request('POST', '/v1/messages', token, request)[0], 202)
        self.change(mode='reject'); self.poll()
        code, result = self.api_request('POST', '/v1/messages', token, request)
        self.assertEqual(code, 202); self.assertEqual(result['status'], 'failed'); self.assertEqual(self.state()['posts'], 1)

    def test_api_rejects_full_queue_and_disabled_service(self):
        token = 'e' * 64
        self.call('save_api_settings', enabled=1, scope='lan', port=8848, transport='http', token=token)
        self.assertEqual(self.api_request('GET', '/v1/devices', token, secure=False)[0], 200)
        for i in range(64): (self.run_dir / 'actions' / f'full-{i}.json').write_text('{}')
        request = dict(device_id=self.device, request_id='new', number='13800000000', text='hello')
        self.assertEqual(self.api_request('POST', '/v1/messages', token, request)[0], 429)
        self.call('save_api_settings', enabled=0, scope='lan', port=8848, transport='http', token='')
        self.assertEqual(self.api_request('GET', '/v1/devices', token, secure=False)[0], 404)

    def test_api_invalid_scope_fails_closed(self):
        token = 'a' * 64
        self.call('save_api_settings', enabled=1, scope='lan', port=8848, token=token)
        config = json.loads(self.config.read_text()); config['api']['scope'] = 'invalid'
        self.config.write_text(json.dumps(config))
        self.assertEqual(self.api_request('GET', '/v1/devices', token)[0], 503)

    def test_webhook_credentials_are_encrypted_and_preserved_on_edit(self):
        config = {'method': 'POST', 'body': '{"text":{{message_json}}}'}
        secrets = {'url': 'http://127.0.0.1:1234/?token=secret-url', 'headers': 'Authorization: Bearer secret-header'}
        created = self.call('save_channel', id=0, name='Webhook', kind='webhook', enabled=1, version=0,
                            config=json.dumps(config), secrets=json.dumps(secrets))
        self.assertNotIn('error', created)
        channels = self.call('channels')['items']
        self.assertNotIn('secret-url', json.dumps(channels))
        self.assertNotIn('secret-header', json.dumps(channels))
        saved = self.config.read_text()
        self.assertNotIn('secret-url', saved)
        self.assertNotIn('secret-header', saved)
        edited = self.call('save_channel', id=created['id'], name='Webhook', kind='webhook', enabled=1,
                           version=channels[0]['version'], config=json.dumps(config), secrets='{}')
        self.assertNotIn('error', edited)

    def test_first_sync_baseline_and_new_message(self):
        self.channel(); self.change(messages=[self.message(1, 1)]); self.poll()
        self.assertEqual(self.state().get('push_attempts', 0), 0)
        self.change(messages=[self.message(1, 1), self.message(2, 2)]); self.poll()
        self.assertEqual(self.state()['push_attempts'], 1)
        self.assertFalse(self.pending())
        self.poll(); self.assertEqual(self.state()['push_attempts'], 1)

    def test_catches_offline_and_startup_burst(self):
        self.channel(); self.change(messages=[self.message(1, 1)]); self.poll()
        self.change(messages=[self.message(i, i) for i in range(1, 8)])
        self.poll(reset=True)
        self.assertEqual(self.state()['push_attempts'], 4)
        self.assertEqual(len(self.pending()), 2)
        self.poll(reset=True); self.assertEqual(self.state()['push_attempts'], 6)
        self.assertFalse(self.pending())

    def test_forward_does_not_truncate_long_message(self):
        self.channel(); self.poll()
        message = 'A' * 9000 + '中文'
        self.change(messages=[self.message(1, 1, message)]); self.poll()
        self.assertIn(message, self.state()['last_push_text'])
        self.assertFalse(self.pending())

    def test_disabling_channel_during_device_read_cancels_forward(self):
        channel = self.channel(); self.poll()
        self.change(mode='slow', status_reads=0, messages=[self.message(1, 1)])
        self.call('sync', device_id=self.device)
        worker = subprocess.Popen([str(BINARY), 'poll'], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            time.sleep(0.5)
            row = self.call('channels')['items'][0]
            result = self.call('save_channel', id=channel, name='disabled', kind='wecom', enabled=0,
                               version=row['version'], config='{}', secrets='{}')
            self.assertNotIn('error', result)
            worker.communicate(timeout=15)
            self.assertEqual(worker.returncode, 0)
            self.assertEqual(self.state().get('push_attempts', 0), 0)
        finally:
            if worker.poll() is None:
                worker.kill(); worker.communicate()

    def test_send_is_not_repeated_after_crash_at_submission(self):
        self.poll()
        self.call('send', device_id=self.device, key='crash-send', number='13800000000', text='test')
        self.change(mode='post_crash')
        self.assertNotEqual(self.run_binary('poll', check=False).returncode, 0)
        self.assertEqual(self.state()['posts'], 1)
        self.change(mode='normal'); self.run_binary('poll')
        self.assertEqual(self.state()['posts'], 1)
        history = self.call('send_history', device_id=self.device)['items']
        self.assertEqual(history[-1]['status'], 'unconfirmed')

    def test_manual_pin_is_not_repeated_after_crash_at_submission(self):
        self.change(modem_state='modem_waitpin'); self.poll()
        iccid = self.call('sim_info', device_id=self.device)['iccid']
        self.call('unlock_pin', device_id=self.device, iccid=iccid, pin='0000', key='crash-pin', force=0)
        self.change(mode='post_crash')
        self.assertNotEqual(self.run_binary('poll', check=False).returncode, 0)
        self.assertEqual(self.state()['posts'], 1)
        self.change(mode='normal'); self.run_binary('poll')
        self.assertEqual(self.state()['posts'], 1)

    def test_auto_pin_attempt_survives_worker_restart(self):
        self.change(modem_state='modem_waitpin'); self.poll()
        iccid = self.call('sim_info', device_id=self.device)['iccid']
        self.call('save_pin', device_id=self.device, iccid=iccid, pin='1234', auto_enabled=1)
        self.change(mode='post_crash')
        self.call('sync', device_id=self.device)
        self.assertNotEqual(self.run_binary('poll', check=False).returncode, 0)
        self.assertEqual(self.state()['posts'], 1)
        self.assertEqual(json.loads(self.config.read_text())['pins'][0]['auto_attempted'], 1)
        self.change(mode='normal'); self.poll(reset=True)
        self.assertEqual(self.state()['posts'], 1)
        self.change(modem_state='modem_init_complete'); self.poll()
        self.assertEqual(json.loads(self.config.read_text())['pins'][0].get('auto_attempted', 0), 0)

    def test_oversized_runtime_is_reported_and_preserved(self):
        path = self.run_dir / 'state.json'
        data = b' ' * (4 * 1024 * 1024 + 1)
        path.write_bytes(data)
        self.assertIn('error', self.call('devices'))
        self.run_binary('poll')
        self.assertEqual(path.read_bytes(), data)

    def test_uci_subprocess_timeout_releases_lock(self):
        helper = self.root / 'hung-uci'
        helper.write_text('#!/bin/sh\nsleep 30\n'); helper.chmod(0o700)
        local_config = self.env.pop('SIM_RELAY_CONFIG')
        self.env['SIM_RELAY_TEST_UCI'] = str(helper)
        self.env['SIM_RELAY_TEST_UCI_TIMEOUT_MS'] = '200'
        start = time.monotonic()
        result = self.run_binary('call', 'status', check=False)
        self.assertLess(time.monotonic() - start, 3)
        self.assertNotEqual(result.returncode, 0)
        self.env['SIM_RELAY_CONFIG'] = local_config
        self.assertTrue(self.call('status')['ok'])

    def test_pending_recovered_even_when_modem_copy_deleted(self):
        self.channel(); self.poll()
        self.change(messages=[self.message(1, 1)], push_mode='timeout'); self.poll()
        self.assertEqual(len(self.pending()), 1)
        job = json.loads(self.pending()[0].read_text()); job['targets'][0]['next_try'] = 0
        self.pending()[0].write_text(json.dumps(job))
        self.change(messages=[], push_mode='normal'); self.poll(reset=True)
        self.assertEqual(self.state()['push_attempts'], 2)
        self.assertIn('新短信', self.state()['last_push_text'])
        self.assertFalse(self.pending())

    def test_latest_second_ties_are_not_missed(self):
        self.channel(); self.change(messages=[self.message(1, 1)]); self.poll()
        self.change(messages=[self.message(1, 1), self.message(2, 1, '同秒')]); self.poll(reset=True)
        self.assertEqual(self.state()['push_attempts'], 1)
        self.poll(reset=True); self.assertEqual(self.state()['push_attempts'], 1)

    def test_reverse_modem_order_uses_max_time(self):
        self.channel(); self.change(messages=[self.message(3, 3), self.message(1, 1)]); self.poll()
        self.change(messages=[self.message(4, 4), self.message(3, 3), self.message(1, 1)]); self.poll(reset=True)
        self.assertEqual(self.state()['push_attempts'], 1)

    def test_idle_poll_does_not_rewrite_progress(self):
        self.channel(); self.change(messages=[self.message(1, 1)]); self.poll()
        p = self.state_dir / f'cursor-{self.device}.json'; before = p.stat().st_mtime_ns
        self.poll(); self.assertEqual(p.stat().st_mtime_ns, before)

    def test_only_latest_time_ids_persisted(self):
        self.channel(); self.change(messages=[self.message(i, i) for i in range(1, 40)]); self.poll()
        cursor = json.loads((self.state_dir / f'cursor-{self.device}.json').read_text())
        self.assertEqual(len(cursor['keys']), 1)
        self.assertNotIn('新短信', json.dumps(cursor, ensure_ascii=False))
        self.assertFalse(list(self.state_dir.glob('*.db')))

    def test_failed_file_write_does_not_advance_progress(self):
        self.channel(); self.poll()
        cursor = self.state_dir / f'cursor-{self.device}.json'; before = cursor.read_bytes()
        # Fill the bounded pending queue with valid unrelated jobs.
        for i in range(1024):
            (self.state_dir / 'pending' / f'full-{i}.json').write_text(json.dumps({'device_id': 999, 'targets': []}))
        self.change(messages=[self.message(1, 1)]); self.poll()
        self.assertEqual(cursor.read_bytes(), before)
        self.assertIn('Cannot persist', self.call('devices')['items'][0]['last_error'])
        # Empty orphan files are removed in bounded batches; the forward follows them.
        for _ in range(10):
            self.poll()
            if self.state().get('push_attempts', 0): break
        self.assertEqual(self.state()['push_attempts'], 1)

    def test_corrupt_progress_is_retained_and_reported(self):
        self.channel(); self.poll(); p = self.state_dir / f'cursor-{self.device}.json'; p.write_text('invalid')
        self.change(messages=[self.message(1, 1)]); self.poll()
        self.assertEqual(p.read_text(), 'invalid')
        self.assertIn('Unreadable progress', self.call('devices')['items'][0]['last_error'])

    def test_channel_edit_cancels_old_pending(self):
        cid = self.channel(); self.poll(); self.change(messages=[self.message(1, 1)], push_mode='timeout'); self.poll()
        row = self.call('channels')['items'][0]
        self.call('save_channel', id=cid, name='修改', kind='wecom', enabled=1, version=row['version'], config='{}', secrets='{}')
        self.poll(); self.assertFalse(self.pending()); self.assertEqual(self.state()['push_attempts'], 1)

    def test_multichannel_success_checkpoint(self):
        first = self.channel(); second = self.channel()
        self.call('bind_channels', device_id=self.device, ids=json.dumps([first, second])); self.poll()
        self.change(messages=[self.message(1, 1)]); self.poll()
        self.assertEqual(self.state()['push_attempts'], 2); self.assertFalse(self.pending())

    def test_successful_channel_not_repeated_when_another_fails(self):
        sms = self.channel(kind='sms'); remote = self.channel()
        self.call('bind_channels', device_id=self.device, ids=json.dumps([sms, remote])); self.poll()
        self.change(messages=[self.message(1, 1)], push_mode='timeout'); self.poll()
        self.assertEqual(self.state()['posts'], 1)
        self.assertEqual(len(self.pending()), 1)
        p = self.pending()[0]; job=json.loads(p.read_text())
        self.assertEqual([t['id'] for t in job['targets']], [remote])
        job['targets'][0]['next_try']=0; p.write_text(json.dumps(job))
        self.change(push_mode='normal'); self.poll(reset=True)
        self.assertFalse(self.pending()); self.assertEqual(self.state()['posts'], 1)

    def test_real_daemon_startup_uses_persistent_progress(self):
        self.channel(); self.change(messages=[self.message(1, 1)]); self.poll()
        self.change(messages=[self.message(1, 1), self.message(2, 2)])
        process=subprocess.Popen([str(BINARY), 'daemon'], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline=time.monotonic()+5
            while time.monotonic()<deadline:
                try:
                    if self.state().get('push_attempts', 0)==1: break
                except json.JSONDecodeError:
                    pass
                time.sleep(.03)
            else:
                self.fail('Daemon did not recover startup SMS')
        finally:
            process.terminate(); stdout,stderr=process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, stderr)
        self.assertEqual(stdout, '')
        self.assertFalse(self.pending())

    def test_new_binding_does_not_replay_history(self):
        self.change(messages=[self.message(1, 1)]); self.poll(); self.channel(); self.poll()
        self.assertEqual(self.state().get('push_attempts', 0), 0)
        self.change(messages=[self.message(1, 1), self.message(2, 2)]); self.poll()
        self.assertEqual(self.state()['push_attempts'], 1)

    def test_sim_change_rebaselines(self):
        self.channel(); self.poll(); self.change(iccid='8986001234567890999', messages=[self.message(1, 1)]); self.poll()
        self.assertEqual(self.state().get('push_attempts', 0), 0)

    def test_send_and_temporary_request_lookup(self):
        self.poll(); args=dict(device_id=self.device, key='send-one', number='13800000000', text='你好')
        self.assertEqual(self.call('send', **args)['status'], 'queued'); self.poll()
        self.assertEqual(self.call('send', **args)['status'], 'sent'); self.assertEqual(self.state()['posts'], 1)

    def test_device_delete_targets_live_slots(self):
        self.change(messages=[self.message(1, 1)]); self.poll()
        mid = self.call('messages', device_id=self.device)['items'][0]['id']
        result = self.call('delete_messages', device_id=self.device, ids=json.dumps([mid]), scope='device', key='delete-1')
        self.assertNotIn('error', result); self.poll()
        self.assertFalse(self.state()['messages'])
        self.assertEqual(self.call('operation_history', device_id=self.device)['items'][0]['status'], 'succeeded')

    def test_keep_recent_messages_prunes_oldest(self):
        self.call('save_device', id=self.device, name='SIM 测试', adapter='zte_mf833u1',
                  base_url='http://192.0.2.1', poll_interval=30, enabled=1, keep_messages=2)
        self.change(messages=[self.message(i, i) for i in range(1, 6)])
        self.poll()
        remaining = self.state()['messages']
        self.assertEqual(sorted(int(m['id']) for m in remaining), [4, 5])
        self.assertEqual(len(self.call('messages', device_id=self.device)['items']), 2)

    def test_save_pin_encrypts_and_unlocks(self):
        self.change(modem_state='modem_waitpin'); self.poll()
        iccid = self.call('sim_info', device_id=self.device)['iccid']
        result = self.call('save_pin', device_id=self.device, iccid=iccid, pin='73195628', auto_enabled=0)
        self.assertNotIn('error', result); self.assertNotIn('73195628', self.config.read_text())
        job = self.call('unlock_pin', device_id=self.device, iccid=iccid, pin='', key='unlock-1', force=0)
        self.assertNotIn('ciphertext', job); self.assertNotIn('pin', job)
        self.poll(); self.assertEqual(self.state()['modem_state'], 'modem_init_complete')

    def test_auto_pin_and_remaining_try_guard(self):
        self.change(modem_state='modem_waitpin'); self.poll(); iccid=self.call('sim_info', device_id=self.device)['iccid']
        self.call('save_pin', device_id=self.device, iccid=iccid, pin='73195628', auto_enabled=1)
        self.poll(); self.assertEqual(self.state()['posts'], 1)
        self.change(modem_state='modem_waitpin', pin_retries=2); self.poll(reset=True)
        self.assertEqual(self.state()['posts'], 1)

    def test_missing_credential_key_not_replaced(self):
        self.channel(); key=self.state_dir / 'credentials.key'; key.unlink()
        cid=self.call('channels')['items'][0]['id']
        result=self.call('save_channel', id=cid, name='更新', kind='wecom', enabled=1, version=1, config='{}', secrets='{}')
        self.assertIn('error', result); self.assertFalse(key.exists())

    def test_public_channel_response_hides_secrets(self):
        self.channel(); response=json.dumps(self.call('channels'))
        self.assertNotIn('abcdefgh1234', response); self.assertNotIn('ciphertext', response)
        self.assertEqual((self.state_dir / 'credentials.key').stat().st_mode & 0o777, 0o600)

    def test_adding_same_zte_endpoint_twice_is_rejected(self):
        duplicate=self.call('save_device',name='duplicate',adapter='zte_mf833u1',base_url='http://192.0.2.1:80/')
        self.assertIn('already been added',duplicate['error'])
        self.assertEqual(len(self.call('devices')['items']),1)
        distinct=self.call('save_device',name='different',adapter='zte_mf833u1',base_url='http://192.0.2.1:8080')
        self.assertNotIn('error',distinct)

    def test_deleted_device_id_not_reused(self):
        self.poll(); self.call('remove_device', device_id=self.device)
        new=self.call('save_device', name='new', adapter='mock')['id']; self.assertGreater(new, self.device)

    def test_remove_device_clears_its_sim_pin_and_progress(self):
        self.change(modem_state='modem_waitpin'); self.poll()
        iccid=self.call('sim_info',device_id=self.device)['iccid']
        self.call('save_pin',device_id=self.device,iccid=iccid,pin='73195628',auto_enabled=0)
        stored=json.loads(self.config.read_text()); self.assertEqual(stored['pins'][0]['device_id'],self.device)
        self.call('remove_device',device_id=self.device)
        self.assertEqual(json.loads(self.config.read_text())['pins'],[])
        self.assertNotIn(self.device,[d['id'] for d in self.call('devices')['items']])
        self.assertFalse((self.state_dir/f'cursor-{self.device}.json').exists())
        replacement=self.call('save_device',name='replacement',adapter='zte_mf833u1',base_url='http://192.0.2.1')['id']
        self.run_binary('poll')
        self.assertFalse(self.call('sim_info',device_id=replacement)['saved'])

    def test_remove_device_preserves_other_sim_pin(self):
        self.change(modem_state='modem_waitpin'); self.poll()
        iccid=self.call('sim_info',device_id=self.device)['iccid']
        self.call('save_pin',device_id=self.device,iccid=iccid,pin='73195628',auto_enabled=0)
        state=json.loads(self.config.read_text())
        state['pins'].append(dict(iccid='8986009999999999999',ciphertext='other',device_id=999))
        self.config.write_text(json.dumps(state))
        self.call('remove_device',device_id=self.device)
        self.assertEqual([p['iccid'] for p in json.loads(self.config.read_text())['pins']],['8986009999999999999'])

    def test_removing_old_device_preserves_moved_sim_pin(self):
        self.change(modem_state='modem_waitpin'); self.poll()
        other = self.call('save_device', name='second', adapter='zte_mf833u1', base_url='http://192.0.2.2')['id']
        self.poll()
        iccid = self.call('sim_info', device_id=other)['iccid']
        self.call('save_pin', device_id=other, iccid=iccid, pin='1234', auto_enabled=0)
        self.assertNotIn('error', self.call('remove_device', device_id=self.device))
        self.assertEqual(json.loads(self.config.read_text())['pins'][0]['device_id'], other)

    def test_remove_pin_with_missing_runtime_and_legacy_pin_record(self):
        self.poll(); iccid=self.call('sim_info',device_id=self.device)['iccid']
        self.call('save_pin',device_id=self.device,iccid=iccid,pin='73195628',auto_enabled=0)
        state=json.loads(self.config.read_text()); state['pins'][0].pop('device_id')
        self.config.write_text(json.dumps(state)); (self.run_dir/'state.json').unlink()
        self.assertNotIn('error',self.call('remove_device',device_id=self.device))
        self.assertEqual(json.loads(self.config.read_text())['pins'],[])

    def test_remove_device_clears_its_pending_jobs_without_waiting_for_daemon(self):
        self.channel(); self.poll()
        self.change(messages=[self.message(1,1)],push_mode='timeout'); self.poll()
        self.assertEqual(len(self.pending()),1)
        self.call('send',device_id=self.device,key='pending-send',number='13800000000',text='pending')
        other=self.state_dir/'pending'/'other.json'; other.write_text(json.dumps({'device_id':999,'targets':[]}))
        self.assertTrue(list((self.run_dir/'actions').glob('*.json')))
        self.assertNotIn('error',self.call('remove_device',device_id=self.device))
        self.assertEqual(self.pending(),[other])
        self.assertFalse(list((self.run_dir/'actions').glob('*.json')))

    def test_remove_pin_by_owner_even_if_sim_and_cursor_unavailable(self):
        self.change(modem_state='modem_waitpin'); self.poll()
        iccid=self.call('sim_info',device_id=self.device)['iccid']
        self.call('save_pin',device_id=self.device,iccid=iccid,pin='73195628',auto_enabled=0)
        (self.run_dir/'state.json').unlink()
        self.assertNotIn('error',self.call('remove_device',device_id=self.device))
        self.assertEqual(json.loads(self.config.read_text())['pins'],[])

    def test_old_local_history_methods_removed(self):
        methods=json.loads(self.run_binary('list').stdout)
        self.assertNotIn('delete_local', methods); self.assertNotIn('acknowledge_send', methods)

    def test_uci_command_configuration_roundtrip(self):
        helper = self.root / 'uci-fixture'
        helper.write_text("""#!/usr/bin/python3
import json, os, sys
from pathlib import Path
p = Path(os.environ['UCI_FIXTURE_FILE'])
command, argument = sys.argv[2:]
if command == 'get':
    print(p.read_text() if p.exists() else '{}')
elif command == 'set':
    assert argument.startswith('sim-relay.main.data=')
    raw = argument.split('=', 1)[1]
    json.loads(raw)
    p.write_text(raw)
elif command == 'commit':
    assert argument == 'sim-relay'
else:
    sys.exit(1)
""")
        helper.chmod(0o700)
        self.env.pop('SIM_RELAY_CONFIG')
        self.env['SIM_RELAY_TEST_UCI'] = str(helper)
        self.env['UCI_FIXTURE_FILE'] = str(self.root / 'uci-value')
        result = self.call('save_device', name="名字 ' ; $(false)", adapter='mock')
        self.assertNotIn('error', result)
        self.assertEqual(self.call('devices')['items'][0]['name'], "名字 ' ; $(false)")
        self.channel()
        raw=(self.root / 'uci-value').read_text()
        self.assertNotIn('abcdefgh1234', raw)

    def test_rpcd_session_metadata_is_accepted_but_not_saved(self):
        token='0123456789abcdef'*2
        self.assertIn('items',self.call('devices',ubus_rpc_session=token))
        added=self.call('save_device',name='browser',adapter='mock',ubus_rpc_session=token)
        self.assertNotIn('error',added)
        self.assertNotIn(token,self.config.read_text())
        self.assertNotIn('ubus_rpc_session',self.config.read_text())
        self.assertIn('error',self.call('devices',unexpected=1,ubus_rpc_session=token))
        self.assertIn('error',self.call('unknown_method',ubus_rpc_session=token))
        self.assertIn('error',self.call('devices',ubus_rpc_session=123))
        raw='{"ubus_rpc_session":"a","ubus_rpc_session":"b"}'
        import subprocess
        result=subprocess.run([str(BINARY),'call','devices'],input=raw,text=True,capture_output=True,env=self.env,check=True)
        self.assertIn('error',json.loads(result.stdout))

    def test_rpc_argument_validation(self):
        for body in ['[]', '{"device_id":"1"}', '{"device_id":1,"device_id":2}', '{"query":"\\u0000"}']:
            result=subprocess.run([str(BINARY), 'call', 'messages'], input=body, text=True,
                                  capture_output=True, env=self.env, check=True)
            self.assertIn('error', json.loads(result.stdout))

    def test_send_key_cannot_silently_reuse_different_content(self):
        self.poll(); args=dict(device_id=self.device,key='same-key',number='13800000000',text='original')
        self.assertNotIn('error',self.call('send',**args))
        for finished in [False,True]:
            if finished: self.poll()
            changed=dict(args,text='different')
            self.assertIn('different SMS content',self.call('send',**changed)['error'])
            self.assertEqual(self.call('send',**args)['status'],'sent' if finished else 'queued')
        self.assertEqual(self.state()['posts'],1)

    def test_sysupgrade_backup_restores_credentials_progress_and_pending(self):
        self.channel(); self.poll()
        self.change(messages=[self.message(1,1)],push_mode='timeout'); self.poll()
        # Simulate only the new keep.d rules, not a whole filesystem backup.
        keep=(ROOT/'package/sim-relay/files/sim-relay.keep').read_text().splitlines()
        restore=self.root/'restored'; restore.mkdir(mode=0o700)
        mapping={'/etc/config/sim-relay':self.config,'/etc/sim-relay':self.state_dir}
        archive=self.root/'upgrade.tar'
        (self.state_dir/'relay.db').write_text('obsolete database')
        with tarfile.open(archive,'w') as tar:
            for rule in keep:
                source=str(mapping['/etc/config/sim-relay']) if rule=='/etc/config/sim-relay' else rule.replace('/etc/sim-relay',str(self.state_dir),1)
                for name in glob.glob(source):
                    suffix='etc/config/sim-relay' if name==str(self.config) else 'etc/sim-relay/'+str(Path(name).relative_to(self.state_dir))
                    tar.add(name,arcname=suffix)
        # The installed package provides the private state directory with mode 0700.
        (restore/'etc/sim-relay').mkdir(parents=True,mode=0o700)
        with tarfile.open(archive) as tar:
            self.assertFalse(any(name.endswith('relay.db') for name in tar.getnames()))
            tar.extractall(restore,filter='data')
        self.state_dir=restore/'etc/sim-relay'
        self.env['SIM_RELAY_STATE']=str(self.state_dir)
        self.env['SIM_RELAY_CONFIG']=str(restore/'etc/config/sim-relay')
        self.env['SIM_RELAY_RUNTIME']=str(restore/'runtime')
        jobfile=self.pending()[0]; job=json.loads(jobfile.read_text()); job['targets'][0]['next_try']=0; jobfile.write_text(json.dumps(job))
        self.change(push_mode='normal'); self.poll()
        self.assertFalse(self.pending()); self.assertEqual(self.state()['push_attempts'],2)
        self.assertEqual(len(self.call('channels')['items']),1)

    def test_sdk_dependencies_exclude_sqlite(self):
        self.assertNotIn('sqlite', (ROOT / 'CMakeLists.txt').read_text().lower())
        self.assertNotIn('sqlite', (ROOT / 'package/sim-relay/Makefile').read_text().lower())
