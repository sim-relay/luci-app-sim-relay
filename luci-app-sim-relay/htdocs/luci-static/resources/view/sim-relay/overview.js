'use strict';
'require view';
'require rpc';
'require ui';
'require poll';

var devices = rpc.declare({ object: 'sim-relay', method: 'devices' });
var messages = rpc.declare({ object: 'sim-relay', method: 'messages',
	params: ['device_id', 'limit', 'offset', 'after_id', 'query', 'direction'] });
var saveDevice = rpc.declare({ object: 'sim-relay', method: 'save_device',
	params: ['id', 'name', 'adapter', 'base_url', 'modem', 'poll_interval', 'enabled'] });
var discover = rpc.declare({ object: 'sim-relay', method: 'discover' });
var removeDevice = rpc.declare({ object: 'sim-relay', method: 'remove_device', params: ['device_id'] });
var syncDevice = rpc.declare({ object: 'sim-relay', method: 'sync', params: ['device_id'] });
var sendSMS = rpc.declare({ object: 'sim-relay', method: 'send', params: ['device_id', 'key', 'number', 'text'] });
var sendHistory = rpc.declare({ object: 'sim-relay', method: 'send_history', params: ['device_id'] });

function checked(result) {
	if (result.error && !result.id) throw new Error(result.error);
	return result;
}
function report(error) {
	ui.addNotification(null, E('p', {}, _(error.message)), 'error');
}

return view.extend({
	handleSaveApply: null,
	handleSave: null,
	handleReset: null,
	load: function() { return devices().then(checked); },
	render: function(data) {
		var selected = 0, offset = 0, writing = L.hasViewPermission() === true;
		var list = E('select', { 'class': 'cbi-input-select' });
		var search = E('input', { 'type': 'text', 'placeholder': _('Search sender or text') });
		var status = E('p');
		var table = E('table', { 'class': 'table' });
		var history = E('table', { 'class': 'table' });
		var sequence = 0;
		var prev = E('button', { 'class': 'btn', 'click': function() { offset = Math.max(0, offset - 50); refresh().catch(report); } }, _('Previous'));
		var next = E('button', { 'class': 'btn', 'click': function() { offset += 50; refresh().catch(report); } }, _('Next'));
		var current = data.items || [];
		function fill(items) {
			current = items;
			list.replaceChildren();
			items.forEach(function(d) { list.appendChild(E('option', { 'value': String(d.id) }, d.name + (d.enabled ? '' : ' (' + _('Disabled') + ')'))); });
			if (!items.some(function(d) { return d.id === selected; })) selected = items.length ? items[0].id : 0;
			list.value = String(selected);
		}
		function refresh() {
			var requestSequence = ++sequence;
			return devices().then(checked).then(function(result) {
				if (requestSequence !== sequence) return null;
				fill(result.items || []);
				var d = current.find(function(item) { return item.id === selected; });
				status.textContent = d ? ((d.last_error ? _(d.last_error) : '') || (d.last_sync ? _('Last sync (UTC):') + ' ' + d.last_sync : _('Waiting for first sync'))) +
					(d.iccid ? ' · ICCID: ' + d.iccid : '') : _('Add a device to get started');
				return selected ? Promise.all([messages(selected, 50, offset, 0, search.value, '').then(checked),
					sendHistory(selected).then(checked)]) : [{ items: [] }, { items: [] }];
			}).then(function(result) {
				if (!result || requestSequence !== sequence) return;
				var sent = result[1]; result = result[0];
				table.replaceChildren(E('tr', { 'class': 'tr table-titles' }, [
					E('th', {}, _('Sender / recipient')), E('th', {}, _('Message')),
					E('th', {}, _('Time')), E('th', {}, _('Direction'))
				]));
				(result.items || []).forEach(function(m) {
					var deviceId = selected;
					table.appendChild(E('tr', { 'class': 'tr' }, [
						E('td', {}, m.number), E('td', { 'style': 'white-space:pre-wrap;overflow-wrap:anywhere' }, m.text),
						E('td', {}, m.timestamp || '—'), E('td', {}, m.direction === 'inbound' ? _('Received') : _('Sent')),

					]));
				});
				prev.disabled = offset === 0; next.disabled = (result.items || []).length < 50;
				history.replaceChildren(E('tr', { 'class': 'tr table-titles' }, [
					E('th', {}, _('Recipient')), E('th', {}, _('Message')), E('th', {}, _('Status')),
					E('th', {}, _('Details'))]));
				(sent.items || []).forEach(function(job) {
					var deviceId = selected;
					history.appendChild(E('tr', { 'class': 'tr' }, [E('td', {}, job.number),
						E('td', { 'style': 'white-space:pre-wrap;overflow-wrap:anywhere' }, job.text),
						E('td', {}, ({ queued: _('Queued'), preparing: _('Preparing'), attempting: _('Submitting'),
							sent: _('Sent'), failed: _('Failed'), unconfirmed: _('Unconfirmed'), cancelled: _('Cancelled') })[job.status] || job.status),
						E('td', {}, (job.error ? _(job.error) : job.created_at)),

					]));
				});
			});
		}
		function compose() {
			var deviceId = selected, d = current.find(function(v) { return v.id === deviceId; });
			if (!d || !d.enabled) { report(new Error(_('Select an enabled device with sending available'))); return; }
			var storageKey = 'sim-relay-send-' + deviceId, draft;
			try {
				draft = JSON.parse(sessionStorage.getItem(storageKey) || 'null');
				if (!draft) {
					var random = new Uint8Array(16); window.crypto.getRandomValues(random);
					draft = { key: Array.from(random).map(function(n) { return n.toString(16).padStart(2, '0'); }).join(''), number: '', text: '', submitted: false };
				}
			} catch (error) { report(error); return; }
			var number = E('input', { 'type': 'tel', 'value': draft.number, 'readonly': (draft.submitted) || null});
			var text = E('textarea', { 'rows': 6, 'style': 'width:100%', 'readonly': (draft.submitted) || null}, draft.text);
			var submit = E('button', { 'class': 'btn cbi-button-positive', 'click': function(ev) {
				var button = ev.target; button.disabled = true;
				draft.number = number.value; draft.text = text.value; draft.submitted = true;
				try { sessionStorage.setItem(storageKey, JSON.stringify(draft)); }
				catch (error) { button.disabled = false; report(error); return; }
				number.readOnly = true; text.readOnly = true;
				sendSMS(deviceId, draft.key, draft.number, draft.text).then(function(result) {
					if (result.error && !result.id) {
						draft.submitted = false; sessionStorage.setItem(storageKey, JSON.stringify(draft));
						number.readOnly = false; text.readOnly = false; throw new Error(result.error);
					}
					sessionStorage.removeItem(storageKey); ui.hideModal(); return refresh();
				}).catch(report).finally(function() { button.disabled = false; });
			} }, draft.submitted ? _('Check original request') : _('Send'));
			ui.showModal(_('Send SMS'), [E('p', {}, d.name),
				E('p', {}, [_('Recipient number'), E('br'), number]),
				E('p', {}, [_('Message'), E('br'), text]),
				E('p', {}, _('ZTE supports up to 335 Unicode BMP characters. Local modems use ModemManager for encoding and segmentation.')),
				E('div', { 'class': 'right' }, [E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ', submit])]);
		}
		function edit(d) {
			d = d || { id: 0, name: '', adapter: 'zte_mf833u1', base_url: 'http://192.168.101.1', poll_interval: 30, enabled: 1 };
			var name = E('input', { 'type': 'text', 'value': d.name, 'maxlength': 128 });
			var adapter = E('select', { 'disabled': (!!d.id) || null}, [E('option', { 'value': 'zte_mf833u1' }, 'ZTE MF833U1'), E('option', { 'value': 'modemmanager' }, _('Local cellular modem (ModemManager)')), E('option', { 'value': 'mock' }, _('Demo device'))]);
			adapter.value = d.adapter;
			var url = E('input', { 'type': 'text', 'value': d.base_url, 'disabled': (!!d.id) || null});
			var modem = E('select', { 'disabled': (!!d.id) || null}, [E('option', { 'value': d.modem || 'any' }, d.modem || 'any')]);
            var scanStatus = E('p');
            var urlRow = E('p', {}, [_('Management URL'), E('br'), url]);
            var modemRow = E('p', {}, [_('Local cellular modem'), E('br'), modem]);
            function scan() {
                return discover().then(checked).then(function(result) {
                    modem.replaceChildren(); (result.items || []).forEach(function(v) {
                        modem.appendChild(E('option', { 'value': v.modem }, v.name + ' · ' + v.hardware_id));
                    });
                    scanStatus.textContent = (result.items || []).length ? '' : _('No modems are available in ModemManager.');
                }).catch(function(error) { scanStatus.textContent = _(error.message); });
            }
            function connectionFields() {
                urlRow.style.display = adapter.value === 'zte_mf833u1' ? '' : 'none';
                modemRow.style.display = adapter.value === 'modemmanager' ? '' : 'none';
                scanStatus.style.display = adapter.value === 'modemmanager' ? '' : 'none';
                url.disabled = !!d.id || adapter.value !== 'zte_mf833u1';
                modem.disabled = !!d.id || adapter.value !== 'modemmanager';
                if (!d.id && adapter.value === 'modemmanager') scan();
            }
            adapter.addEventListener('change', connectionFields); connectionFields();
            var interval = E('input', { 'type': 'number', 'min': 10, 'max': 86400, 'value': d.poll_interval });
			var enabled = E('input', { 'type': 'checkbox', 'checked': (!!d.enabled) || null});
			ui.showModal(d.id ? _('Edit device') : _('Add device'), [
				E('p', {}, [_('Name'), E('br'), name]), E('p', {}, [_('Adapter'), E('br'), adapter]),
				urlRow, modemRow, scanStatus,
				E('p', {}, [_('Poll interval (seconds)'), E('br'), interval]),
				E('p', {}, [enabled, ' ', _('Enabled')]),
				E('div', { 'class': 'right' }, [E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
					E('button', { 'class': 'btn cbi-button-positive', 'click': function(ev) {
						ev.target.disabled = true;
						saveDevice(d.id, name.value, adapter.value, adapter.value === 'zte_mf833u1' ? url.value : '', modem.value || 'any',
							Number(interval.value), enabled.checked ? 1 : 0).then(checked).then(function(result) {
							selected = result.id; offset = 0; ui.hideModal(); return refresh();
						}).catch(report).finally(function() { ev.target.disabled = false; });
					} }, _('Save'))])]);
		}
		fill(current);
		list.addEventListener('change', function() { selected = Number(list.value); offset = 0; refresh().catch(report); });
		search.addEventListener('change', function() { offset = 0; refresh().catch(report); });
		poll.add(function() { return refresh().catch(report); }, 5);
		refresh().catch(report);
		return E('div', {}, [E('h2', {}, 'SIM Relay'),
			E('p', {}, _('Manage devices and browse SMS stored on the modem.')),
			E('div', { 'class': 'cbi-section' }, [list, ' ',
				E('button', { 'class': 'btn cbi-button-add', 'disabled': (!writing) || null, 'click': function() { edit(); } }, _('Add device')), ' ',
				E('button', { 'class': 'btn', 'disabled': (!writing) || null, 'click': function() { var d = current.find(function(v) { return v.id === selected; }); if (d) edit(d); } }, _('Edit device')), ' ',
				E('button', { 'class': 'btn cbi-button-remove', 'disabled': (!writing) || null, 'click': function() {
                    var deviceId = selected;
                    if (!deviceId) return;
                    ui.showModal(_('Remove device'), [E('p', {}, _('Remove this device, its saved SIM PIN and pending forwards.')),
                        E('div', { 'class': 'right' }, [E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
                            E('button', { 'class': 'btn cbi-button-negative', 'click': function(ev) {
                                ev.target.disabled = true;
                                removeDevice(deviceId).then(checked).then(function(result) { if (result.warning) report(new Error(result.warning)); selected = 0; offset = 0; ui.hideModal(); return refresh(); })
                                    .catch(report).finally(function() { ev.target.disabled = false; });
                            } }, _('Remove device'))])]);
                } }, _('Remove device')), ' ',
				E('button', { 'class': 'btn cbi-button-action', 'disabled': (!writing) || null, 'click': function() {
					if (selected) syncDevice(selected).then(checked).then(function() { status.textContent = _('Sync queued'); }).catch(report);
				} }, _('Sync now')), ' ', E('button', { 'class': 'btn cbi-button-positive', 'disabled': (!writing) || null, 'click': compose }, _('Send SMS'))]),
			status, search, table, E('div', {}, [prev, ' ', next]), E('h3', {}, _('Recent sends (cleared on restart)')), history]);
	}
});
