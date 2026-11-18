'use strict';
'require view';
'require rpc';
'require ui';
'require poll';

var devices = rpc.declare({ object: 'sim-relay', method: 'devices' });
var messages = rpc.declare({ object: 'sim-relay', method: 'messages',
	params: ['device_id', 'limit', 'offset', 'after_id', 'query', 'direction'] });
var sendSMS = rpc.declare({ object: 'sim-relay', method: 'send', params: ['device_id', 'key', 'number', 'text'] });
var deleteMessages = rpc.declare({ object: 'sim-relay', method: 'delete_messages', params: ['device_id', 'ids', 'scope', 'key'] });
var syncDevice = rpc.declare({ object: 'sim-relay', method: 'sync', params: ['device_id'] });

function checked(result) {
	if (result.error && !result.id) throw new Error(result.error);
	return result;
}
function report(error) {
	ui.addNotification(null, E('p', {}, _(error.message)), 'error');
}
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

return view.extend({
	handleSaveApply: null,
	handleSave: null,
	handleReset: null,
	load: function() { return devices().then(checked); },
	render: function(data) {
		var selected = 0, offset = 0, sequence = 0, writing = L.hasViewPermission() === true;
		var deviceItems = data.items || [], selection = new Set();
		var list = E('select', { 'class': 'cbi-input-select' });
		var search = E('input', { 'class': 'cbi-input-text', 'type': 'search', 'placeholder': _('Search sender or text') });
		var summary = E('div');
		var table = E('table', { 'class': 'table sim-relay-messages' });
		var prev = E('button', { 'class': 'btn', 'click': function() { offset = Math.max(0, offset - 50); refresh().catch(report); } }, _('Previous'));
		var next = E('button', { 'class': 'btn', 'click': function() { offset += 50; refresh().catch(report); } }, _('Next'));
		var remove = E('button', { 'class': 'btn cbi-button-negative', 'disabled': true, 'click': function() {
			if (!selection.size || !selected) { report(new Error(_('Select messages first.'))); return; }
			var deviceId = selected, ids = JSON.stringify(Array.from(selection)), key = requestKey();
			ui.showModal(_('Delete messages'), [
				E('p', {}, _('Delete these messages from the device? This cannot be undone.')),
				E('div', { 'class': 'right' }, [E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
					E('button', { 'class': 'btn cbi-button-negative', 'click': function(ev) {
						ev.target.disabled = true;
						deleteMessages(deviceId, ids, 'device', key).then(checked).then(function() {
							selection.clear(); updateActions(); ui.hideModal(); return syncDevice(deviceId);
						}).then(function() { return refresh(); }).catch(report).finally(function() { ev.target.disabled = false; });
					} }, _('Delete selected messages'))])
			]);
		} }, _('Delete selected messages'));

		function current() {
			return deviceItems.find(function(item) { return item.id === selected; });
		}
		function updateActions() {
			remove.disabled = !writing || !selected || !selection.size;
		}
		function fill(items) {
			deviceItems = items;
			list.replaceChildren();
			items.forEach(function(d) {
				list.appendChild(E('option', { 'value': String(d.id) }, display(d.name) + (d.enabled ? '' : ' (' + _('Disabled') + ')')));
			});
			if (!items.some(function(d) { return d.id === selected; })) { selected = items.length ? items[0].id : 0; selection.clear(); }
			list.value = String(selected);
		}
		function renderSummary(d) {
			if (!d) {
				summary.replaceChildren(E('p', {}, _('Add a device to get started')));
				return;
			}
			var sync = d.last_error ? _(d.last_error) :
				(d.last_sync ? _('Last sync (UTC):') + ' ' + d.last_sync : _('Waiting for first sync'));
			summary.replaceChildren(
				E('p', {}, [E('strong', {}, display(d.name)), ' · ', display(d.adapter), ' · ', d.enabled ? _('Enabled') : _('Disabled')]),
				E('p', {}, [sync, d.iccid ? ' · ICCID: ' + d.iccid : '']),
				E('p', {}, _('Poll interval (seconds)') + ': ' + display(d.poll_interval) + ' · ' +
					_('Keep recent messages') + ': ' + (d.keep_messages > 0 ? String(d.keep_messages) : '—'))
			);
		}
		function renderMessages(items) {
			var all = items.length > 0 && items.every(function(m) { return selection.has(m.id); });
			var selectAll = E('input', { 'type': 'checkbox', 'checked': all || null, 'disabled': (!writing || !items.length) || null});
			selectAll.addEventListener('change', function() {
				items.forEach(function(m) { if (selectAll.checked) selection.add(m.id); else selection.delete(m.id); });
				updateActions();
			});
			table.replaceChildren(E('tr', { 'class': 'tr table-titles' }, [
				E('th', {}, selectAll), E('th', {}, _('Sender / recipient')), E('th', {}, _('Message')),
				E('th', {}, _('Time')), E('th', {}, _('Direction'))
			]));
			if (!items.length) {
				table.appendChild(emptyRow(5, selected ? _('No messages found.') : _('Add a device to get started')));
				updateActions();
				return;
			}
			items.forEach(function(m) {
				var box = E('input', { 'type': 'checkbox', 'checked': (selection.has(m.id)) || null, 'disabled': (!writing) || null});
				box.addEventListener('change', function() { if (box.checked) selection.add(m.id); else selection.delete(m.id); updateActions(); });
				table.appendChild(E('tr', { 'class': 'tr' }, [
					E('td', {}, box), E('td', {}, display(m.number)),
					E('td', { 'style': 'white-space:pre-wrap;overflow-wrap:anywhere' }, m.text == null ? '' : m.text),
					E('td', {}, m.timestamp || '—'),
					E('td', {}, m.direction === 'inbound' ? _('Received') : _('Sent'))
				]));
			});
			updateActions();
		}
		function refresh() {
			var requestSequence = ++sequence;
			return devices().then(checked).then(function(result) {
				if (requestSequence !== sequence) return null;
				fill(result.items || []);
				renderSummary(current());
				if (!selected) {
					renderMessages([]);
					prev.disabled = true;
					next.disabled = true;
					return null;
				}
				return messages(selected, 50, offset, 0, search.value, '').then(checked);
			}).then(function(result) {
				if (!result || requestSequence !== sequence) return;
				var items = result.items || [];
				renderMessages(items);
				prev.disabled = offset === 0;
				next.disabled = items.length < 50;
			});
		}
		function compose() {
			var deviceId = selected, d = current();
			if (!d || !d.enabled) { report(new Error(_('Select an enabled device with sending available'))); return; }
			var storageKey = 'sim-relay-send-' + deviceId, draft;
			try {
				draft = JSON.parse(sessionStorage.getItem(storageKey) || 'null');
				if (!draft) {
					var random = new Uint8Array(16); window.crypto.getRandomValues(random);
					draft = { key: Array.from(random).map(function(n) { return n.toString(16).padStart(2, '0'); }).join(''), number: '', text: '', submitted: false };
				}
			} catch (error) { report(error); return; }
			var number = E('input', { 'class': 'cbi-input-text', 'type': 'tel', 'value': draft.number, 'readonly': (draft.submitted) || null});
			var text = E('textarea', { 'class': 'cbi-input-textarea', 'rows': 6, 'readonly': (draft.submitted) || null}, draft.text);
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
			ui.showModal(_('Send SMS'), [E('p', {}, display(d.name)),
				field(_('Recipient number'), number),
				field(_('Message'), text, E('div', { 'class': 'sim-relay-help' },
					_('ZTE supports up to 335 Unicode BMP characters. Local modems use ModemManager for encoding and segmentation.'))),
				E('div', { 'class': 'right' }, [E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ', submit])]);
		}
		fill(deviceItems);
		list.addEventListener('change', function() {
			selected = Number(list.value); offset = 0; selection.clear(); refresh().catch(report);
		});
		search.addEventListener('change', function() { offset = 0; selection.clear(); refresh().catch(report); });
		search.addEventListener('keydown', function(ev) { if (ev.key === 'Enter') { offset = 0; selection.clear(); refresh().catch(report); } });
		poll.add(function() { return refresh().catch(report); }, 5);
		refresh().catch(report);
		return E('div', { 'class': 'sim-relay' }, [
			E('link', { 'rel': 'stylesheet', 'href': L.resource('sim-relay/style.css') }),
			E('h2', {}, _('Messages')),
			E('p', {}, _('Browse, send and delete SMS on the selected device.')),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Messages')),
				E('div', { 'class': 'cbi-section-node' }, [
					field(_('Device'), list),
					summary,
					E('div', { 'class': 'sim-relay-actions' }, [
						E('button', { 'class': 'btn cbi-button-positive', 'disabled': (!writing) || null, 'click': compose }, _('Send SMS')), ' ',
						remove
					]),
					field(_('Search sender or text'), search),
					E('div', { 'class': 'sim-relay-table-wrap' }, [table]),
					E('div', { 'class': 'sim-relay-actions' }, [prev, ' ', next])
				])
			])
		]);
	}
});
