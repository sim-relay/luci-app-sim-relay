'use strict';
'require view';
'require rpc';
'require ui';
'require poll';

var devices = rpc.declare({ object: 'sim-relay', method: 'devices' });
var saveDevice = rpc.declare({ object: 'sim-relay', method: 'save_device',
	params: ['id', 'name', 'adapter', 'base_url', 'modem', 'poll_interval', 'enabled', 'keep_messages'] });
var discover = rpc.declare({ object: 'sim-relay', method: 'discover' });
var removeDevice = rpc.declare({ object: 'sim-relay', method: 'remove_device', params: ['device_id'] });
var simInfo = rpc.declare({ object: 'sim-relay', method: 'sim_info', params: ['device_id'] });
var savePin = rpc.declare({ object: 'sim-relay', method: 'save_pin', params: ['device_id', 'iccid', 'pin', 'auto_enabled', 'forget'] });
var unlockPin = rpc.declare({ object: 'sim-relay', method: 'unlock_pin', params: ['device_id', 'iccid', 'pin', 'key', 'force'] });

function checked(result) { if (result.error && !result.id) throw new Error(result.error); return result; }
function report(error) { ui.addNotification(null, E('p', {}, _(error.message)), 'error'); }
function requestKey() {
	var bytes = new Uint8Array(16); window.crypto.getRandomValues(bytes);
	return Array.from(bytes).map(function(n) { return n.toString(16).padStart(2, '0'); }).join('');
}
function display(value) { return value == null || value === '' ? '—' : String(value); }
function field(label, control, help) {
	return E('div', { 'class': 'cbi-value sim-relay-field' }, [
		E('label', { 'class': 'cbi-value-title' }, label),
		E('div', { 'class': 'cbi-value-field' }, help == null ? [control] : [control, help])
	]);
}
function emptyRow(columns, text) {
	return E('tr', { 'class': 'tr' }, [
		E('td', { 'colspan': columns, 'class': 'sim-relay-empty' }, text)
	]);
}
function adapterName(adapter) {
	return adapter === 'zte_mf833u1' ? 'ZTE MF833U1' : adapter === 'modemmanager' ? _('Local cellular modem (ModemManager)') : adapter === 'mock' ? _('Demo device') : display(adapter);
}

return view.extend({
	handleSave: null, handleSaveApply: null, handleReset: null,
	load: function() { return devices().then(checked); },
	render: function(initial) {
		var writable = L.hasViewPermission() === true, sequence = 0;
		var deviceItems = initial.items || [], simItems = [];
		var deviceTable = E('table', { 'class': 'table' });
		var simTable = E('table', { 'class': 'table' });

		function renderDevices() {
			deviceTable.replaceChildren(E('tr', { 'class': 'tr table-titles' }, [
				E('th', {}, _('Name')), E('th', {}, _('Adapter')), E('th', {}, _('Status')),
				E('th', {}, _('Poll interval (seconds)')), E('th', {}, _('Keep recent messages')), E('th', {}, _('Actions'))
			]));
			if (!deviceItems.length) {
				deviceTable.appendChild(emptyRow(6, _('Add a device to get started')));
				return;
			}
			deviceItems.forEach(function(d) {
				var status = d.enabled ? _('Enabled') : _('Disabled');
				if (d.last_error) status += ' · ' + _(d.last_error);
				deviceTable.appendChild(E('tr', { 'class': 'tr' }, [
					E('td', {}, display(d.name)), E('td', {}, adapterName(d.adapter)), E('td', {}, status),
					E('td', {}, display(d.poll_interval)),
					E('td', {}, d.keep_messages > 0 ? String(d.keep_messages) : '—'),
					E('td', {}, [
						E('button', { 'class': 'btn', 'disabled': (!writable) || null, 'click': function() { editDevice(d); } }, _('Edit')), ' ',
						E('button', { 'class': 'btn cbi-button-remove', 'disabled': (!writable) || null, 'click': function() {
							ui.showModal(_('Remove device'), [
								E('p', {}, _('Remove this device, its saved SIM PIN and pending forwards.')),
								E('div', { 'class': 'right' }, [E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
									E('button', { 'class': 'btn cbi-button-negative', 'click': function(ev) {
										ev.target.disabled = true;
										removeDevice(d.id).then(checked).then(function(result) {
											if (result.warning) report(new Error(result.warning));
											ui.hideModal(); return refreshAll();
										}).catch(report).finally(function() { ev.target.disabled = false; });
									} }, _('Remove device'))])
							]);
						} }, _('Remove'))
					])
				]));
			});
		}
		function renderSims() {
			simTable.replaceChildren(E('tr', { 'class': 'tr table-titles' }, [
				E('th', {}, _('Device')), E('th', {}, 'ICCID'), E('th', {}, _('Status')),
				E('th', {}, _('PIN attempts')), E('th', {}, _('Enable automatic unlock')), E('th', {}, _('Actions'))
			]));
			if (!deviceItems.length) {
				simTable.appendChild(emptyRow(6, _('Add a device first.')));
				return;
			}
			if (!simItems.length) {
				simTable.appendChild(emptyRow(6, _('Waiting for sync')));
				return;
			}
			simItems.forEach(function(row) {
				var d = row.device, info = row.info, error = row.error;
				if (!info) {
					simTable.appendChild(E('tr', { 'class': 'tr' }, [
						E('td', {}, display(d.name)), E('td', {}, '—'), E('td', {}, _(error || 'Waiting for sync')),
						E('td', {}, '—'), E('td', {}, '—'), E('td', {}, '—')
					]));
					return;
				}
				var state = info.sim || {}, iccid = info.iccid || state.iccid || '';
				var labels = { none: _('Unlocked'), 'sim-pin': _('PIN required'), 'sim-puk': _('PUK required'), unknown: _('Unknown state') };
				var locked = state.unlock_required === 'sim-pin';
				var status = (labels[state.unlock_required] || _('Waiting for sync')) + ' · ' + (info.saved ? _('PIN saved and encrypted') : _('No saved PIN'));
				if (state.pin_helper_available === false) status += ' · ' + _('Install the optional sim-relay-modemmanager package to unlock a local modem with a PIN.');
				var automatic = E('input', { 'type': 'checkbox', 'checked': (!!info.auto_enabled) || null,
					'disabled': (!writable || !info.saved || !iccid) || null});
				automatic.addEventListener('change', function() {
					savePin(d.id, iccid, '', automatic.checked ? 1 : 0, 0).then(checked).then(function() {
						return refreshSims();
					}).catch(function(err) { automatic.checked = !automatic.checked; report(err); });
				});
				simTable.appendChild(E('tr', { 'class': 'tr' }, [
					E('td', {}, display(d.name)),
					E('td', {}, iccid || '—'),
					E('td', {}, status),
					E('td', {}, typeof state.pin_retries === 'number' && state.pin_retries >= 0 ? String(state.pin_retries) : _('Unknown')),
					E('td', {}, automatic),
					E('td', {}, [
						E('button', { 'class': 'btn cbi-button-positive', 'disabled': (!writable || !iccid) || null,
							'click': function() { editPin(d, info); } }, _('Set PIN')), ' ',
						E('button', { 'class': 'btn cbi-button-action', 'disabled': (!writable || !iccid || !locked) || null,
							'click': function() { unlock(d, info); } }, _('Unlock once'))
					])
				]));
			});
		}
		function editDevice(d) {
			d = d || { id: 0, name: '', adapter: 'zte_mf833u1', base_url: 'http://192.168.101.1',
				poll_interval: 30, enabled: 1, keep_messages: 0 };
			var name = E('input', { 'class': 'cbi-input-text', 'type': 'text', 'value': d.name, 'maxlength': 128 });
			var adapter = E('select', { 'class': 'cbi-input-select', 'disabled': (!!d.id) || null}, [
				E('option', { 'value': 'zte_mf833u1' }, 'ZTE MF833U1'),
				E('option', { 'value': 'modemmanager' }, _('Local cellular modem (ModemManager)')),
				E('option', { 'value': 'mock' }, _('Demo device'))
			]);
			adapter.value = d.adapter;
			var url = E('input', { 'class': 'cbi-input-text', 'type': 'text', 'value': d.base_url, 'disabled': (!!d.id) || null});
			var modem = E('select', { 'class': 'cbi-input-select', 'disabled': (!!d.id) || null}, [E('option', { 'value': d.modem || 'any' }, d.modem || 'any')]);
			var scanStatus = E('div', { 'class': 'sim-relay-help' });
			var urlRow = field(_('Management URL'), url);
			var modemRow = field(_('Local cellular modem'), modem, scanStatus);
			function scan() {
				return discover().then(checked).then(function(result) {
					modem.replaceChildren();
					(result.items || []).forEach(function(v) {
						modem.appendChild(E('option', { 'value': v.modem }, display(v.name) + ' · ' + display(v.hardware_id)));
					});
					scanStatus.textContent = (result.items || []).length ? '' : _('No modems are available in ModemManager.');
				}).catch(function(error) { scanStatus.textContent = _(error.message); });
			}
			function connectionFields() {
				urlRow.style.display = adapter.value === 'zte_mf833u1' ? '' : 'none';
				modemRow.style.display = adapter.value === 'modemmanager' ? '' : 'none';
				url.disabled = !!d.id || adapter.value !== 'zte_mf833u1';
				modem.disabled = !!d.id || adapter.value !== 'modemmanager';
				if (!d.id && adapter.value === 'modemmanager') scan();
			}
			adapter.addEventListener('change', connectionFields); connectionFields();
			var interval = E('input', { 'class': 'cbi-input-text', 'type': 'number', 'min': 10, 'max': 86400, 'value': d.poll_interval == null ? 30 : d.poll_interval });
			var keep = E('input', { 'class': 'cbi-input-text', 'type': 'number', 'min': 0, 'max': 1000, 'value': d.keep_messages || 0 });
			var enabled = E('input', { 'type': 'checkbox', 'checked': (!!d.enabled) || null});
			ui.showModal(d.id ? _('Edit device') : _('Add device'), [
				field(_('Name'), name),
				field(_('Adapter'), adapter),
				urlRow,
				modemRow,
				field(_('Poll interval (seconds)'), interval),
				field(_('Keep recent messages'), keep, E('div', { 'class': 'sim-relay-help' }, [
					_('Automatic cleanup keeps the newest N messages and removes older ones.'), E('br'), _('0 = keep all messages')
				])),
				E('div', { 'class': 'cbi-value sim-relay-field' }, [
					E('div', { 'class': 'cbi-value-title' }, _('Status')),
					E('div', { 'class': 'cbi-value-field' }, [enabled, ' ', _('Enabled')])
				]),
				E('div', { 'class': 'right' }, [E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
					E('button', { 'class': 'btn cbi-button-positive', 'click': function(ev) {
						ev.target.disabled = true;
						saveDevice(d.id, name.value, adapter.value, adapter.value === 'zte_mf833u1' ? url.value : '', modem.value || 'any',
							Number(interval.value), enabled.checked ? 1 : 0, Number(keep.value)).then(checked).then(function() {
							ui.hideModal(); return refreshAll();
						}).catch(report).finally(function() { ev.target.disabled = false; });
					} }, _('Save'))])
			]);
		}
		function editPin(d, info) {
			var iccid = info.iccid || (info.sim || {}).iccid || '';
			var pin = E('input', { 'class': 'cbi-input-text', 'type': 'password', 'inputmode': 'numeric', 'maxlength': 8, 'autocomplete': 'new-password' });
			var save = E('button', { 'class': 'btn cbi-button-positive', 'disabled': true, 'click': function(ev) {
				ev.target.disabled = true;
				savePin(d.id, iccid, pin.value, info.auto_enabled ? 1 : 0, 0).then(checked).then(function() {
					pin.value = ''; ui.hideModal(); return refreshSims();
				}).catch(report).finally(function() { ev.target.disabled = false; });
			} }, _('Save'));
			pin.addEventListener('input', function() { save.disabled = !/^[0-9]{4,8}$/.test(pin.value); });
			ui.showModal(_('Set PIN'), [
				E('p', {}, display(d.name) + ' · ICCID: ' + (iccid || '—')),
				field(_('PIN (4-8 digits)'), pin),
				E('div', { 'class': 'right' }, [E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ', save])
			]);
		}
		function unlock(d, info) {
			var iccid = info.iccid || (info.sim || {}).iccid || '';
			var pin = E('input', { 'class': 'cbi-input-text', 'type': 'password', 'inputmode': 'numeric', 'maxlength': 8, 'autocomplete': 'new-password' });
			var force = E('input', { 'type': 'checkbox' });
			var key = requestKey();
			ui.showModal(_('Unlock once'), [
				E('p', {}, display(d.name) + ' · ICCID: ' + (iccid || '—')),
				field(_('PIN (4-8 digits; leave blank to unlock with the saved PIN)'), pin),
				E('div', { 'class': 'cbi-value sim-relay-field' }, [
					E('div', { 'class': 'cbi-value-title' }, _('Allow one attempt when fewer than 3 PIN attempts remain')),
					E('div', { 'class': 'cbi-value-field' }, [force])
				]),
				E('div', { 'class': 'right' }, [E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
					E('button', { 'class': 'btn cbi-button-action', 'click': function(ev) {
						ev.target.disabled = true;
						unlockPin(d.id, iccid, pin.value, key, force.checked ? 1 : 0).then(checked).then(function() {
							pin.value = ''; ui.hideModal(); return refreshSims();
						}).catch(report).finally(function() { ev.target.disabled = false; });
					} }, _('Unlock once'))])
			]);
		}
		function refreshDevices() {
			return devices().then(checked).then(function(result) {
				deviceItems = result.items || [];
				renderDevices();
				return deviceItems;
			});
		}
		function refreshSims() {
			var serial = ++sequence;
			return refreshDevices().then(function(items) {
				if (serial !== sequence) return null;
				if (!items.length) { simItems = []; renderSims(); return null; }
				return Promise.all(items.map(function(d) {
					return simInfo(d.id).then(checked).then(function(info) { return { device: d, info: info }; })
						.catch(function(error) { return { device: d, error: error.message }; });
				}));
			}).then(function(result) {
				if (serial !== sequence) return;
				if (result) simItems = result;
				renderSims();
			});
		}
		function refreshAll() { return refreshSims(); }
		poll.add(function() { return refreshSims().catch(report); }, 10);
		refreshSims().catch(report);
		return E('div', { 'class': 'sim-relay' }, [
			E('link', { 'rel': 'stylesheet', 'href': L.resource('sim-relay/style.css') }),
			E('h2', {}, _('Devices and SIM')),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Devices')),
				E('div', { 'class': 'cbi-section-node' }, [
					E('div', { 'class': 'sim-relay-actions' }, [
						E('button', { 'class': 'btn cbi-button-add', 'disabled': (!writable) || null, 'click': function() { editDevice(); } }, _('Add device'))
					]),
					E('div', { 'class': 'sim-relay-table-wrap' }, [deviceTable])
				])
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('SIM')),
				E('div', { 'class': 'cbi-section-node' }, [E('div', { 'class': 'sim-relay-table-wrap' }, [simTable])])
			])
		]);
	}
});
