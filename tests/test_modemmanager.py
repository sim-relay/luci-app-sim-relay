"""Run the real subprocess adapter against an offline mmcli executable."""
import json
from pathlib import Path
from test_lite import LiteCase, ROOT

class ModemManagerTests(LiteCase):
    def setUp(self):
        super().setUp()
        self.mmfile=self.root / 'mm.json'
        self.mmfile.write_text(json.dumps({'messages': []}))
        self.env['SIM_RELAY_MM_FIXTURE']=str(self.mmfile)
        self.env['SIM_RELAY_TEST_MMCLI']=str(ROOT / 'tests/mmcli_fixture.py')
        self.env['SIM_RELAY_TEST_PIN_HELPER']=str(ROOT / 'tests/mmcli_fixture.py')
        self.device=self.call('save_device', name='本机蜂窝', adapter='modemmanager', modem='any')['id']

    def mm(self): return json.loads(self.mmfile.read_text())
    def mm_change(self, **kw):
        v=self.mm(); v.update(kw); self.mmfile.write_text(json.dumps(v))
    def sms(self, slot, second, text='中文😀'):
        return dict(path=f'/org/freedesktop/ModemManager1/SMS/{slot}', number='10086', text=text,
                    state='received', timestamp=f'2026-11-14T12:00:{second:02}+08:00')

    def test_missing_mm_rejects_add_without_changing_configuration(self):
        self.env['SIM_RELAY_TEST_MMCLI']='/nonexistent/mmcli'
        count=len(self.call('devices')['items'])
        result=self.call('save_device', name='missing', adapter='modemmanager', modem='any')
        self.assertIn('Install the modemmanager package', result['error'])
        self.assertEqual(len(self.call('devices')['items']),count)
        self.assertTrue(self.call('status')['ok'])

    def test_duplicate_physical_modem_rejected(self):
        result=self.call('save_device',name='duplicate',adapter='modemmanager',modem='any')
        self.assertIn('already been added',result['error'])

    def test_missing_mm_does_not_block_other_devices(self):
        self.env['SIM_RELAY_TEST_MMCLI']='/nonexistent/mmcli'
        result=self.call('save_device',name='demo',adapter='mock')
        self.assertNotIn('error',result)

    def test_missing_pin_helper_fails_without_submission(self):
        self.env['SIM_RELAY_TEST_PIN_HELPER']='/nonexistent/helper'
        self.mm_change(locked=True); self.poll(); iccid=self.call('sim_info',device_id=self.device)['iccid']
        self.call('unlock_pin',device_id=self.device,iccid=iccid,pin='1234',key='pin-missing',force=0); self.poll()
        job=self.call('operation_history',device_id=self.device)['items'][0]
        self.assertEqual(job['status'],'failed'); self.assertIn('sim-relay-modemmanager',job['error'])
        self.assertEqual(self.mm().get('pin_calls',0),0)

    def test_no_forced_dependency(self):
        for name in ('package/sim-relay/Makefile','luci-app-sim-relay/Makefile','package/sim-relay-modemmanager/Makefile'):
            for line in (ROOT/name).read_text().splitlines():
                if 'DEPENDS:=' in line: self.assertNotIn('+modemmanager',line)

    def test_discovery_and_sim(self):
        found=self.call('discover')['items'][0]
        self.assertEqual(found['hardware_id'],'mm:123456789012345')
        self.poll(); info=self.call('sim_info',device_id=self.device)
        self.assertEqual(info['iccid'],'8986001234567890123')
        self.assertEqual(info['sim']['pin_retries'],3)

    def test_path_renumbering_uses_hardware_binding(self):
        self.mm_change(messages=[self.sms(1,1)]); self.poll()
        identity=self.call('messages',device_id=self.device)['items'][0]['external_id']
        self.mm_change(index=5,messages=[self.sms(77,1)]); self.poll(reset=True)
        self.assertEqual(self.call('messages',device_id=self.device)['items'][0]['external_id'],identity)

    def test_other_hardware_does_not_replace_binding(self):
        self.mm_change(equipment='555555555555555'); self.poll()
        d=next(v for v in self.call('devices')['items'] if v['id']==self.device)
        self.assertIn('absent',d['last_error'])

    def test_multiple_modems_require_selection(self):
        self.mm_change(second=True)
        result=self.call('save_device',name='ambiguous',adapter='modemmanager',modem='any')
        self.assertIn('error',result)
        result=self.call('save_device',name='selected',adapter='modemmanager',modem='/org/freedesktop/ModemManager1/Modem/99')
        self.assertNotIn('error',result)

    def test_unicode_send_file_and_lookup(self):
        self.poll(); args=dict(device_id=self.device,key='send',number='+8613800000000',text="中文😀 ' , \n第二行")
        self.assertEqual(self.call('send',**args)['status'],'queued'); self.poll()
        self.assertEqual(self.call('send',**args)['status'],'sent')
        s=self.mm(); self.assertEqual(s['sends'],1); self.assertEqual(s['messages'][0]['text'],args['text'])
        self.assertEqual(s['text_mode'],0o600); self.assertFalse(Path(s['text_file']).exists())

    def test_send_failure_not_automatically_retried(self):
        self.poll(); self.call('send',device_id=self.device,key='fail',number='13800000000',text='test')
        self.mm_change(send_failure=True); self.poll(); self.poll()
        self.assertEqual(self.mm()['sends'],1)
        self.assertEqual(self.call('send_history',device_id=self.device)['items'][0]['status'],'unconfirmed')

    def test_delete_device_message(self):
        self.mm_change(messages=[self.sms(1,1)]); self.poll()
        mid=self.call('messages',device_id=self.device)['items'][0]['id']
        self.call('delete_messages',device_id=self.device,ids=json.dumps([mid]),scope='device',key='delete')
        self.poll(); self.assertFalse(self.mm()['messages']); self.assertEqual(self.mm()['deletes'],1)

    def test_pin_uses_stdin_and_original_retry_guards(self):
        self.mm_change(locked=True); self.poll(); iccid=self.call('sim_info',device_id=self.device)['iccid']
        self.call('save_pin',device_id=self.device,iccid=iccid,pin='1234',auto_enabled=0)
        self.call('unlock_pin',device_id=self.device,iccid=iccid,pin='',key='pin',force=0); self.poll()
        self.assertFalse(self.mm()['locked']); self.assertEqual(self.mm()['pin_calls'],1)
        self.assertNotIn('1234',json.dumps(self.mm()['calls']))

    def test_pin2_counter_not_used_and_low_tries_blocked(self):
        self.mm_change(locked=True,tries=2); self.poll(); iccid=self.call('sim_info',device_id=self.device)['iccid']
        self.call('unlock_pin',device_id=self.device,iccid=iccid,pin='1234',key='pin',force=0); self.poll()
        self.assertEqual(self.mm().get('pin_calls',0),0)

    def test_successful_pin_with_mmcli_dash_lock_and_disabled_state(self):
        self.mm_change(locked=True,unlocked_lock='--',unlocked_state='disabled'); self.poll()
        iccid=self.call('sim_info',device_id=self.device)['iccid']
        self.call('unlock_pin',device_id=self.device,iccid=iccid,pin='1234',key='pin-dash',force=0)
        self.poll()
        self.assertEqual(self.call('operation_history',device_id=self.device)['items'][0]['status'],'succeeded')
        self.assertEqual(self.call('sim_info',device_id=self.device)['sim']['unlock_required'],'none')
        self.assertEqual(self.mm()['pin_calls'],1)
        self.assertEqual(self.mm().get('enable_calls'),1)
        self.assertEqual(next(d for d in self.call('devices')['items'] if d['id']==self.device)['last_error'],'')

    def test_enable_disabled_unlocked_modem_and_refresh_sms(self):
        self.mm_change(unlocked_lock='--',unlocked_state='disabled',messages=[self.sms(1,1)]); self.poll()
        self.assertEqual(self.mm()['enable_calls'],1)
        self.assertEqual(len(self.call('messages',device_id=self.device)['items']),1)
        self.assertEqual(self.mm().get('pin_calls',0),0)

    def test_enable_failure_keeps_error_and_locked_sim_is_not_enabled(self):
        self.mm_change(unlocked_lock='--',unlocked_state='disabled',enable_fails=True); self.poll()
        d=next(d for d in self.call('devices')['items'] if d['id']==self.device)
        self.assertIn('Unable to enable',d['last_error'])
        self.mm_change(locked=True,enable_calls=0); self.poll()
        self.assertEqual(self.mm()['enable_calls'],0)
        self.assertEqual(self.mm().get('pin_calls',0),0)

    def test_dash_lock_is_not_unlocked_in_unknown_state(self):
        self.mm_change(reported_lock='--',unlocked_state='unknown'); self.poll()
        self.assertEqual(self.call('sim_info',device_id=self.device)['sim']['unlock_required'],'unknown')

    def test_forwarding_and_offline_recovery_reuse_cursor(self):
        self.channel(); self.mm_change(messages=[self.sms(1,1)]); self.poll()
        self.mm_change(messages=[self.sms(22,1),self.sms(23,2)]); self.poll(reset=True)
        self.assertEqual(self.state()['push_attempts'],1); self.assertFalse(self.pending())
        self.poll(reset=True); self.assertEqual(self.state()['push_attempts'],1)

    def test_incomplete_multipart_does_not_advance_cursor(self):
        self.channel(); self.poll(); m=self.sms(1,1); m['state']='receiving'; self.mm_change(messages=[m]); self.poll()
        self.assertEqual(self.state().get('push_attempts',0),0)
        m['state']='received'; self.mm_change(messages=[m]); self.poll()
        self.assertEqual(self.state()['push_attempts'],1)
