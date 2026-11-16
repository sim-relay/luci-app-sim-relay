'use strict';
'require view';
'require rpc';
'require ui';
'require poll';

var devices = rpc.declare({ object: 'sim-relay', method: 'devices' });
var simInfo = rpc.declare({ object: 'sim-relay', method: 'sim_info', params: ['device_id'] });
var savePin = rpc.declare({ object: 'sim-relay', method: 'save_pin', params: ['device_id', 'iccid', 'pin', 'auto_enabled', 'forget'] });
var unlockPin = rpc.declare({ object: 'sim-relay', method: 'unlock_pin', params: ['device_id', 'iccid', 'pin', 'key', 'force'] });
var messages = rpc.declare({ object: 'sim-relay', method: 'messages', params: ['device_id', 'limit', 'offset', 'after_id', 'query', 'direction'] });
var deleteMessages = rpc.declare({ object: 'sim-relay', method: 'delete_messages', params: ['device_id', 'ids', 'scope', 'key'] });
var history = rpc.declare({ object: 'sim-relay', method: 'operation_history', params: ['device_id'] });
var remove = rpc.declare({ object: 'sim-relay', method: 'remove_device', params: ['device_id'] });
var sync = rpc.declare({ object: 'sim-relay', method: 'sync', params: ['device_id'] });

function checked(result) { if (result.error && !result.id) throw new Error(result.error); return result; }
function report(error) { ui.addNotification(null, E('p', {}, _(error.message)), 'error'); }
function requestKey() {
	var bytes = new Uint8Array(16); window.crypto.getRandomValues(bytes);
	return Array.from(bytes).map(function(n) { return n.toString(16).padStart(2, '0'); }).join('');
}

return view.extend({
	handleSave: null, handleSaveApply: null, handleReset: null,
	load: function() { return devices().then(checked); },
	render: function(initial) {
		var writable = L.hasViewPermission() === true, selected = 0, sequence = 0;
		var deviceList = initial.items || [], selection = new Set(), pinKey = null;
		var select = E('select', { 'class': 'cbi-input-select' });
		var sim = E('div'), rows = E('table', { 'class': 'table' }), operations = E('table', { 'class': 'table' });
		function confirm(title, text, action) {
			ui.showModal(title, [E('p', {}, text), E('div', { 'class': 'right' }, [
				E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
				E('button', { 'class': 'btn cbi-button-action', 'click': function(ev) {
					ev.target.disabled = true;
					Promise.resolve().then(action).then(checked).then(function(result) { if (result.warning) report(new Error(result.warning)); ui.hideModal(); return refresh(); })
						.catch(report).finally(function() { ev.target.disabled = false; });
				} }, _('Confirm'))])]);
		}
		function renderSIM(d, info) {
			var state = info.sim || {}, deviceId = d.id, iccid = info.iccid;
			var pin = E('input', { 'type': 'password', 'inputmode': 'numeric', 'maxlength': 8, 'autocomplete': 'new-password' });
			var automatic = E('input', { 'type': 'checkbox', 'checked': (!!info.auto_enabled) || null});
			var force = E('input', { 'type': 'checkbox' });
			var locked = state.unlock_required === 'sim-pin';
			var labels = { none: _('Unlocked'), 'sim-pin': _('PIN required'), 'sim-puk': _('PUK required'), unknown: _('Unknown state') };
			sim.replaceChildren(E('p', {}, 'ICCID: ' + (iccid || '—') + ' · ' + (labels[state.unlock_required] || _('Waiting for sync'))),
				E('p', {}, _('PIN attempts remaining: ') + (state.pin_retries >= 0 ? state.pin_retries : _('Unknown')) +
					' · ' + (info.saved ? _('PIN saved and encrypted') : _('No saved PIN'))),
				E('p', {}, !writable ? _('This session has read-only access. Sign in with an account that can manage SIM Relay.') : !iccid ? _('Waiting for SIM identification. Refresh device status before saving a PIN.') : ''),
				E('p', {}, state.pin_helper_available === false ? _('Install the optional sim-relay-modemmanager package to unlock a local modem with a PIN.') : ''),
                E('p', {}, [ _('PIN (4-8 digits; leave blank to unlock with the saved PIN)'), E('br'), pin ]),
				E('p', {}, [automatic, ' ', _('Enable automatic unlock'), ' ', force, ' ', _('Allow one attempt when fewer than 3 PIN attempts remain')]),
				E('div', {}, [
					E('button', { 'class': 'btn cbi-button-positive', 'disabled': (!writable || !iccid) || null, 'click': function() {
                        savePin(deviceId, iccid, pin.value, automatic.checked ? 1 : 0, 0).then(checked).then(function() { pin.value = ''; editingPin = false; return refresh(); }).catch(report);
					} }, _('Save PIN settings')), ' ',
					E('button', { 'class': 'btn', 'disabled': (!writable || !info.saved) || null, 'click': function() {
						confirm(_('Forget PIN'), _('Delete the saved PIN for this SIM and disable automatic unlock.'), function() { return savePin(deviceId, iccid, '', 0, 1).then(function(result) { if (!result.error) { editingPin = false; pin.value = ''; } return result; }); });
					} }, _('Forget PIN')), ' ',
					E('button', { 'class': 'btn cbi-button-action', 'disabled': (!writable || !locked) || null, 'click': function(ev) {
						if (!pinKey) pinKey = requestKey();
						var key = pinKey; ev.target.disabled = true;
						unlockPin(deviceId, iccid, pin.value, key, force.checked ? 1 : 0).then(checked).then(function() {
							pinKey = null; pin.value = ''; editingPin = false; return refresh();
						}).catch(report).finally(function() { ev.target.disabled = false; });
					} }, _('Unlock once'))]));
			// Do not poll-replace a PIN while it is being entered.
			pin.addEventListener('input', function() { editingPin = !!pin.value; });
		}
		var editingPin = false, previousSim = '';
		function refresh() {
			var serial = ++sequence;
			return devices().then(checked).then(function(result) {
				if (serial !== sequence) return null;
				deviceList = result.items || [];
				if (!deviceList.some(function(d) { return d.id === selected; })) { selected = deviceList.length ? deviceList[0].id : 0; selection.clear(); }
				select.replaceChildren();
				deviceList.forEach(function(d) { select.appendChild(E('option', { 'value': String(d.id) }, d.name)); });
				select.value = String(selected);
				return selected ? Promise.all([simInfo(selected).then(checked), messages(selected, 100, 0, 0, '', '').then(checked), history(selected).then(checked)]) : null;
			}).then(function(result) {
				if (serial !== sequence) return;
				if (!result) { sim.replaceChildren(E('p', {}, _('Add a device first.'))); rows.replaceChildren(); operations.replaceChildren(); return; }
				var d = deviceList.find(function(item) { return item.id === selected; }), info = result[0], deviceId = selected;
				var signature = JSON.stringify([selected, info]);
				if ((!editingPin || info.iccid !== previousIccid) && signature !== previousSim) {
					renderSIM(d, info); previousSim = signature; previousIccid = info.iccid; editingPin = false;
				}
				var visible = new Set((result[1].items || []).map(function(m) { return m.id; }));
				selection.forEach(function(id) { if (!visible.has(id)) selection.delete(id); });
				rows.replaceChildren(E('tr', { 'class': 'tr' }, [E('th', {}, _('Select')), E('th', {}, _('Number')), E('th', {}, _('Messages')), E('th', {}, _('Time'))]));
				(result[1].items || []).forEach(function(m) {
					var checkbox = E('input', { 'type': 'checkbox', 'checked': (selection.has(m.id)) || null, 'disabled': (!writable) || null});
					checkbox.addEventListener('change', function() { if (checkbox.checked) selection.add(m.id); else selection.delete(m.id); });
					rows.appendChild(E('tr', { 'class': 'tr' }, [E('td', {}, checkbox), E('td', {}, m.number),
						E('td', { 'style': 'white-space:pre-wrap;overflow-wrap:anywhere' }, m.text), E('td', {}, m.timestamp || '—')]));
				});
				operations.replaceChildren(E('tr', { 'class': 'tr' }, [E('th', {}, _('Actions')), E('th', {}, _('Status')), E('th', {}, _('Details'))]));
				(result[2].items || []).forEach(function(job) {
					operations.appendChild(E('tr', { 'class': 'tr' }, [E('td', {}, job.kind === 'pin' ? _('PIN unlock') : _('Delete device SMS')),
						E('td', {}, ({queued: _('Queued'), preparing: _('Preparing'), attempting: _('Submitted; awaiting confirmation'), succeeded: _('Confirmed successful'), failed: _('Failed'), unconfirmed: _('Unconfirmed'), cancelled: _('Cancelled')})[job.status] || job.status),
						E('td', {}, (job.error ? _(job.error) : ''))]));
				});
			});
		}
		var previousIccid = '';
		select.addEventListener('change', function() {
			selected = Number(select.value); selection.clear(); pinKey = null; editingPin = false; previousSim = ''; refresh().catch(report);
		});

		poll.add(function() { return refresh().catch(report); }, 5); refresh().catch(report);
		return E('div', {}, [E('h2', {}, _('SIM and SMS cleanup')), select, ' ', E('button', { 'class': 'btn', 'disabled': (!writable) || null, 'click': function() {
			if (selected) sync(selected).then(checked).then(refresh).catch(report);
		} }, _('Refresh device status')), E('h3', {}, _('SIM / PIN')), sim,
			E('h3', {}, _('Device SMS (up to 100 messages)')), rows, ' ',
			E('button', { 'class': 'btn cbi-button-negative', 'disabled': (!writable) || null, 'click': function() {
				if (!selection.size) { report(new Error(_('Select messages first.'))); return; }
				var id = selected, ids = JSON.stringify(Array.from(selection)), target = 'device', key = requestKey();
				confirm(_('Delete messages'), _('Delete these messages from the device? This cannot be undone.'),
					function() { return deleteMessages(id, ids, target, key).then(function(result) { if (!result.error) selection.clear(); return result; }); });
			} }, _('Delete selected messages')), ' ',
			E('button', { 'class': 'btn cbi-button-remove', 'disabled': (!writable) || null, 'click': function() {
				var id = selected; if (!id) return;
				confirm(_('Remove device'), _('Remove this device, its saved SIM PIN and pending forwards.'), function() { return remove(id); });
			} }, _('Remove device')), E('h3', {}, _('Recent device operations (cleared on restart)')), operations]);
	}
});
