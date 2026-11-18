'use strict';
'require view';
'require rpc';
'require ui';
'require poll';

var listChannels = rpc.declare({ object: 'sim-relay', method: 'channels' });
var listDevices = rpc.declare({ object: 'sim-relay', method: 'devices' });
var bindings = rpc.declare({ object: 'sim-relay', method: 'device_channels', params: ['device_id'] });
var saveBindings = rpc.declare({ object: 'sim-relay', method: 'bind_channels', params: ['device_id', 'ids'] });
var saveChannel = rpc.declare({ object: 'sim-relay', method: 'save_channel', params: ['id', 'name', 'kind', 'config', 'secrets', 'enabled', 'version'] });
var removeChannel = rpc.declare({ object: 'sim-relay', method: 'remove_channel', params: ['id', 'version'] });
var clearCredentials = rpc.declare({ object: 'sim-relay', method: 'clear_channel_credentials', params: ['id', 'version'] });
var testChannel = rpc.declare({ object: 'sim-relay', method: 'test_channel', params: ['device_id', 'channel_id', 'key'] });
var pushHistory = rpc.declare({ object: 'sim-relay', method: 'push_history', params: ['device_id'] });

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

var names = { sms: _('SIM SMS'), telegram: 'Telegram', wecom: _('WeCom'), feishu: _('Feishu'), smtp: _('SMTP email'), webhook: 'Webhook' };
var fields = {
	sms: [['number', _('Forwarding number')]],
	telegram: [['token', _('Bot Token'), 'secret'], ['chat_id', _('Chat ID')]],
	wecom: [['webhook_url', _('Webhook URL'), 'secret']],
	feishu: [['webhook_url', _('Webhook URL'), 'secret'], ['signing_enabled', _('Enable signing'), 'checkbox'], ['signing_secret', _('Signing secret'), 'secret']],
	smtp: [['host', _('SMTP host')], ['port', _('Port'), 'number'], ['from', _('From address')], ['to', _('To address')],
		['username', _('Username')], ['password', _('Password'), 'secret'], ['tls_mode', _('TLS mode'), 'tls']],
	webhook: [['url', _('Webhook URL'), 'secret'], ['method', _('Request method'), 'method'],
		['headers', _('Request headers'), 'secret_multiline'], ['body', _('Request body'), 'multiline']]
};

return view.extend({
	handleSave: null, handleSaveApply: null, handleReset: null,
	load: function() { return Promise.all([listChannels().then(checked), listDevices().then(checked)]); },
	render: function(initial) {
		var writable = L.hasViewPermission() === true, sequence = 0;
		var channels = initial[0].items || [], devices = initial[1].items || [];
		var bound = {}, dirty = {};
		var channelTable = E('table', { 'class': 'table' });
		var associationTable = E('table', { 'class': 'table sim-relay-associations' });
		var historyTable = E('table', { 'class': 'table' });

		function confirm(title, text, callback) {
			ui.showModal(title, [E('p', {}, text), E('div', { 'class': 'right' }, [
				E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
				E('button', { 'class': 'btn cbi-button-action', 'click': function(ev) {
					ev.target.disabled = true;
					Promise.resolve().then(callback).then(checked).then(function() { ui.hideModal(); return refresh(); }).catch(report)
						.finally(function() { ev.target.disabled = false; });
				} }, _('Confirm'))])]);
		}
		function boundTo(deviceId, channelId) {
			return bound[deviceId] && bound[deviceId].has(channelId);
		}
		function renderChannels() {
			channelTable.replaceChildren(E('tr', { 'class': 'tr table-titles' }, [
				E('th', {}, _('Name')), E('th', {}, _('Type')), E('th', {}, _('Status')), E('th', {}, _('Actions'))
			]));
			if (!channels.length) {
				channelTable.appendChild(emptyRow(4, _('No channels have been added.')));
				return;
			}
			channels.forEach(function(c) {
				var testDevice = devices.find(function(d) { return boundTo(d.id, c.id); });
				channelTable.appendChild(E('tr', { 'class': 'tr' }, [
					E('td', {}, display(c.name)),
					E('td', {}, names[c.kind] || display(c.kind)),
					E('td', {}, (c.enabled ? _('Enabled') : _('Disabled')) +
						(c.kind === 'sms' ? '' : ' · ' + (c.has_credentials ? _('Credentials saved') : _('No saved credentials')))),
					E('td', {}, [
						E('button', { 'class': 'btn', 'disabled': (!writable) || null, 'click': function() { edit(c); } }, _('Edit')), ' ',
						E('button', { 'class': 'btn', 'disabled': (!writable || !testDevice || !c.enabled) || null, 'click': function() {
							var key = requestKey();
							confirm(_('Send test'), c.kind === 'sms' ?
								_('Send a test SMS to the forwarding number using the selected device SIM.') :
								_('Send a test message to this channel.'),
								function() { return testChannel(testDevice.id, c.id, key); });
						} }, _('Test')), ' ',
						E('button', { 'class': 'btn', 'disabled': (!writable || !c.has_credentials) || null, 'click': function() {
							confirm(_('Clear credentials'), _('Clear this shared channel credentials, disable the channel and cancel pending forwards.'),
								function() { return clearCredentials(c.id, c.version); });
						} }, _('Clear credentials')), ' ',
						E('button', { 'class': 'btn cbi-button-remove', 'disabled': (!writable) || null, 'click': function() {
							confirm(_('Remove shared channel'), _('Stop using this channel on all linked devices and cancel pending forwards.'),
								function() { return removeChannel(c.id, c.version); });
						} }, _('Remove'))
					])
				]));
			});
		}
		function renderAssociations() {
			associationTable.replaceChildren(E('tr', { 'class': 'tr table-titles' }, [
				E('th', {}, _('Device')), E('th', {}, _('Channel')), E('th', {}, _('Actions'))
			]));
			if (!devices.length) {
				associationTable.appendChild(emptyRow(3, _('Add a device first.')));
				return;
			}
			if (!channels.length) {
				associationTable.appendChild(emptyRow(3, _('No channels have been added.')));
				return;
			}
			devices.forEach(function(d) {
				if (!bound[d.id]) bound[d.id] = new Set();
				var choices = {};
				channels.forEach(function(c) {
					choices[String(c.id)] = display(c.name) + (c.enabled ? '' : ' (' + _('Disabled') + ')');
				});
				var dropdown = new ui.Dropdown(Array.from(bound[d.id], String), choices, {
					multiple: true, optional: true, disabled: !writable
				});
				var selector = dropdown.render();
				selector.addEventListener('cbi-dropdown-change', function() {
					if (!writable) return;
					var selected = new Set(dropdown.getValue().map(Number));
					if (selected.size !== bound[d.id].size || Array.from(selected).some(function(id) { return !bound[d.id].has(id); })) {
						bound[d.id] = selected;
						dirty[d.id] = true;
					}
				});
				associationTable.appendChild(E('tr', { 'class': 'tr' }, [
					E('td', {}, [E('strong', {}, display(d.name)), E('br'), display(d.adapter)]),
					E('td', {}, [selector]),
					E('td', {}, [
						E('button', { 'class': 'btn cbi-button-positive', 'disabled': (!writable) || null, 'click': function() {
							saveBindings(d.id, JSON.stringify(Array.from(bound[d.id]))).then(checked).then(function() {
								dirty[d.id] = false; return refresh();
							}).catch(report);
						} }, _('Save'))
					])
				]));
			});
		}
		function renderHistory(items) {
			var states = { queued: _('Queued / waiting to retry'), attempting: _('Submitting'), waiting_send: _('Waiting for SIM send'),
				sent: _('Sent'), failed: _('Failed'), unconfirmed: _('Unconfirmed'), cancelled: _('Cancelled') };
			historyTable.replaceChildren(E('tr', { 'class': 'tr table-titles' }, [
				E('th', {}, _('Device')), E('th', {}, _('Channel')), E('th', {}, _('Status')),
				E('th', {}, _('Attempts')), E('th', {}, _('Details'))
			]));
			if (!items.length) {
				historyTable.appendChild(emptyRow(5, _('No forwards yet.')));
				return;
			}
			items.forEach(function(job) {
				historyTable.appendChild(E('tr', { 'class': 'tr' }, [
					E('td', {}, display(job.device_name)), E('td', {}, display(job.channel)),
					E('td', {}, states[job.status] || display(job.status)),
					E('td', {}, display(job.attempts == null ? 0 : job.attempts)), E('td', {}, job.error ? _(job.error) : display(job.created_at))
				]));
			});
		}
		function refresh() {
			var serial = ++sequence;
			return Promise.all([listChannels().then(checked), listDevices().then(checked)]).then(function(result) {
				if (serial !== sequence) return null;
				channels = result[0].items || [];
				devices = result[1].items || [];
				var available = new Set(channels.map(function(c) { return c.id; }));
				Object.keys(bound).forEach(function(id) {
					if (!devices.some(function(d) { return String(d.id) === String(id); })) { delete bound[id]; delete dirty[id]; return; }
					bound[id].forEach(function(cid) { if (!available.has(cid)) bound[id].delete(cid); });
				});
				return Promise.all(devices.map(function(d) {
					return Promise.all([
						bindings(d.id).then(checked).catch(function() { return { items: [] }; }),
						pushHistory(d.id).then(checked).catch(function() { return { items: [] }; })
					]).then(function(row) { return { id: d.id, name: d.name, items: row[0].items || [], history: row[1].items || [] }; });
				}));
			}).then(function(result) {
				if (!result || serial !== sequence) return;
				if (associationTable.querySelector('.cbi-dropdown[open]')) return;
				var history = [];
				result.forEach(function(row) {
					if (!dirty[row.id]) bound[row.id] = new Set(row.items.map(function(b) { return b.channel_id; }));
					row.history.forEach(function(item) {
						var copy = JSON.parse(JSON.stringify(item));
						copy.device_name = row.name; history.push(copy);
					});
				});
				renderChannels();
				renderAssociations();
				renderHistory(history);
			});
		}
		function edit(row) {
			row = row || { id: 0, name: '', kind: 'telegram', config_json: '{}', enabled: 1, version: 0 };
			var config = {};
			try { config = JSON.parse(row.config_json || '{}'); } catch (error) { config = {}; }
			var inputs = {}, panel = E('div'), clearHeaders;
			var name = E('input', { 'class': 'cbi-input-text', 'value': row.name, 'maxlength': 128 });
			var kind = E('select', { 'class': 'cbi-input-select', 'disabled': (!!row.id) || null}, Object.keys(names).map(function(key) {
				return E('option', { 'value': key }, names[key]);
			}));
			kind.value = row.kind;
			var enabled = E('input', { 'type': 'checkbox', 'checked': (!!row.enabled) || null});
			function renderFields() {
				inputs = {}; panel.replaceChildren();
				clearHeaders = null;
				var bodyRow = null;
				var specs = (fields[kind.value] || []).concat([['prefix', _('Message prefix')]]);
				specs.forEach(function(spec) {
					var key = spec[0], type = spec[2], value = config[key], input;
					if (type === 'method') {
						input = E('select', { 'class': 'cbi-input-select' }, ['GET', 'POST', 'PUT'].map(function(method) {
							return E('option', { 'value': method }, method);
						}));
						input.value = value || 'POST';
						input.addEventListener('change', function() { if (bodyRow) bodyRow.style.display = input.value === 'GET' ? 'none' : ''; });
					} else if (type === 'tls') {
						input = E('select', { 'class': 'cbi-input-select' }, [E('option', { 'value': 'starttls' }, 'STARTTLS'), E('option', { 'value': 'tls' }, _('Implicit TLS'))]);
						input.value = value || 'starttls';
					} else if (type === 'multiline' || type === 'secret_multiline') {
						input = E('textarea', { 'class': 'cbi-input-textarea', 'rows': 4,
							'placeholder': type === 'secret_multiline' && row.id ? _('Leave blank to keep existing credentials') : null }, value || '');
					} else {
						input = E('input', {
							'class': type === 'checkbox' ? null : 'cbi-input-text',
							'type': type === 'secret' ? 'password' : type === 'number' ? 'number' : type === 'checkbox' ? 'checkbox' : 'text',
							'autocomplete': type === 'secret' ? 'new-password' : 'off'
						});
						if (type === 'checkbox') input.checked = !!value;
						else if (type === 'secret') input.placeholder = row.id ? _('Leave blank to keep existing credentials') : '';
						else input.value = value == null ? (key === 'port' ? 587 : '') : value;
					}
					inputs[key] = { element: input, type: type };
					var help = null;
					if (kind.value === 'webhook' && key === 'url') help = E('div', { 'class': 'sim-relay-help' }, _('Use {{message}} in the URL for URL-encoded message text.'));
					if (kind.value === 'webhook' && key === 'headers') {
						help = E('div', { 'class': 'sim-relay-help' }, [_('Enter one header per line, for example Content-Type: application/json.')]);
						if (row.id) {
							clearHeaders = E('input', { 'type': 'checkbox' });
							help.appendChild(E('div', {}, [E('label', {}, [clearHeaders, ' ', _('Clear saved request headers')])]));
						}
					}
					if (kind.value === 'webhook' && key === 'body') help = E('div', { 'class': 'sim-relay-help' }, _('Use {{message}} for plain text, or {{message_json}} for a JSON string including quotes.'));
					var fieldRow = field(spec[1], input, help);
					if (kind.value === 'webhook' && key === 'body') {
						bodyRow = fieldRow;
						bodyRow.style.display = inputs.method.element.value === 'GET' ? 'none' : '';
					}
					panel.appendChild(fieldRow);
				});
			}
			kind.addEventListener('change', function() { config = {}; renderFields(); }); renderFields();
			ui.showModal(row.id ? _('Edit channel') : _('Add channel'), [
				field(_('Name'), name), field(_('Type'), kind), panel,
				E('div', { 'class': 'cbi-value sim-relay-field' }, [
					E('div', { 'class': 'cbi-value-title' }, _('Status')),
					E('div', { 'class': 'cbi-value-field' }, [enabled, ' ', _('Enabled')])
				]),
				E('div', { 'class': 'right' }, [E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
					E('button', { 'class': 'btn cbi-button-positive', 'click': function(ev) {
						var values = {}, secrets = {}; ev.target.disabled = true;
						Object.keys(inputs).forEach(function(key) {
							var input = inputs[key];
							if (input.type === 'secret' || input.type === 'secret_multiline') {
								if (key === 'headers' && clearHeaders && clearHeaders.checked) secrets[key] = '';
								else if (input.element.value) secrets[key] = input.element.value;
							}
							else values[key] = input.type === 'number' ? Number(input.element.value) :
								input.type === 'checkbox' ? (input.element.checked ? 1 : 0) : input.element.value;
						});
						saveChannel(row.id, name.value, kind.value, JSON.stringify(values), JSON.stringify(secrets), enabled.checked ? 1 : 0, row.version)
							.then(checked).then(function() {
								Object.keys(inputs).forEach(function(key) { if (inputs[key].type === 'secret' || inputs[key].type === 'secret_multiline') inputs[key].element.value = ''; });
								ui.hideModal(); return refresh();
							}).catch(report).finally(function() { ev.target.disabled = false; });
					} }, _('Save'))])
			]);
		}
		poll.add(function() {
			if (associationTable.querySelector('.cbi-dropdown[open]') || Object.keys(dirty).some(function(id) { return dirty[id]; })) return;
			return refresh().catch(report);
		}, 5);
		refresh().catch(report);
		return E('div', { 'class': 'sim-relay' }, [
			E('link', { 'rel': 'stylesheet', 'href': L.resource('sim-relay/style.css') }),
			E('h2', {}, _('Push channels')),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Shared push channels')),
				E('div', { 'class': 'cbi-section-node' }, [
					E('div', { 'class': 'sim-relay-actions' }, [
						E('button', { 'class': 'btn cbi-button-add', 'disabled': (!writable) || null, 'click': function() { edit(); } }, _('Add channel'))
					]),
					E('div', { 'class': 'sim-relay-table-wrap' }, [channelTable])
				])
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Device bindings')),
				E('div', { 'class': 'cbi-section-node' }, [
					E('p', {}, _('New bindings start from a history baseline. Existing progress resumes after restart. Failed forwards retry every minute.')),
					E('div', { 'class': 'sim-relay-table-wrap' }, [associationTable])
				])
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Pending and recent forwards (successful records cleared on restart)')),
				E('div', { 'class': 'cbi-section-node' }, [E('div', { 'class': 'sim-relay-table-wrap' }, [historyTable])])
			])
		]);
	}
});
