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
var names = { sms: _('SIM SMS'), telegram: 'Telegram', wecom: _('WeCom'), feishu: _('Feishu'), smtp: _('SMTP email') };
var fields = {
	sms: [['number', _('Forwarding number')]],
	telegram: [['chat_id', _('Chat ID')]],
	wecom: [['webhook_url', _('Webhook URL'), 'secret']],
	feishu: [['webhook_url', _('Webhook URL'), 'secret'], ['signing_enabled', _('Enable signing'), 'checkbox'], ['signing_secret', _('Signing secret'), 'secret']],
	smtp: [['host', _('SMTP host')], ['port', _('Port'), 'number'], ['from', _('From address')], ['to', _('To address')],
		['username', _('Username')], ['password', _('Password'), 'secret'], ['tls_mode', _('TLS mode'), 'tls']]
};

return view.extend({
	handleSave: null, handleSaveApply: null, handleReset: null,
	load: function() { return Promise.all([listChannels().then(checked), listDevices().then(checked)]); },
	render: function(initial) {
		var writable = L.hasViewPermission() === true, selected = 0, channels = initial[0].items || [], devices = initial[1].items || [];
		var sequence = 0, dirty = false, chosen = new Set();
		var deviceSelect = E('select', { 'class': 'cbi-input-select' });
		var channelTable = E('table', { 'class': 'table' }), bindingBox = E('div'), historyTable = E('table', { 'class': 'table' });
		function confirm(title, text, callback) {
			ui.showModal(title, [E('p', {}, text), E('div', { 'class': 'right' }, [
				E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
				E('button', { 'class': 'btn cbi-button-action', 'click': function(ev) {
					ev.target.disabled = true;
					Promise.resolve().then(callback).then(checked).then(function() { ui.hideModal(); return refresh(); }).catch(report)
						.finally(function() { ev.target.disabled = false; });
				} }, _('Confirm'))])]);
		}
		function edit(row) {
			row = row || { id: 0, name: '', kind: 'telegram', config_json: '{}', enabled: 1, version: 0 };
			var config = JSON.parse(row.config_json), inputs = {}, panel = E('div');
			var name = E('input', { 'value': row.name, 'maxlength': 128 });
			var kind = E('select', { 'disabled': (!!row.id) || null}, Object.keys(names).map(function(key) { return E('option', { 'value': key }, names[key]); }));
			kind.value = row.kind;
			var enabled = E('input', { 'type': 'checkbox', 'checked': (!!row.enabled) || null});
			function renderFields() {
				inputs = {}; panel.replaceChildren();
				if (kind.value === 'telegram') fields.telegram = [['token', _('Bot Token'), 'secret'], ['chat_id', _('Chat ID')]];
				var specs = fields[kind.value].concat([['prefix', _('Message prefix')]]);
				specs.forEach(function(spec) {
					var key = spec[0], type = spec[2], value = config[key], input;
					if (type === 'tls') input = E('select', {}, [E('option', { 'value': 'starttls' }, 'STARTTLS'), E('option', { 'value': 'tls' }, _('Implicit TLS'))]);
					else input = E('input', { 'type': type === 'secret' ? 'password' : type === 'number' ? 'number' : type === 'checkbox' ? 'checkbox' : 'text',
						'autocomplete': type === 'secret' ? 'new-password' : 'off' });
					if (type === 'checkbox') input.checked = !!value;
					else if (type === 'secret') input.placeholder = row.id ? _('Leave blank to keep existing credentials') : '';
					else input.value = value == null ? (key === 'port' ? 587 : key === 'retry_count' ? 3 : key === 'tls_mode' ? 'starttls' : '') : value;
					inputs[key] = { element: input, type: type };
					panel.appendChild(E('p', {}, [spec[1], E('br'), input]));
				});
				if (kind.value === 'smtp') panel.appendChild(E('p', {}, _('SMTP requires the optional sim-relay-smtp package, which provides a separate mail client with TLS support.')));
			}
			kind.addEventListener('change', function() { config = {}; renderFields(); }); renderFields();
			ui.showModal(row.id ? _('Edit channel') : _('Add channel'), [E('p', {}, [_('Name'), E('br'), name]),
				E('p', {}, [_('Type'), E('br'), kind]), panel, E('p', {}, [enabled, ' ', _('Enabled')]),
				E('div', { 'class': 'right' }, [E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
					E('button', { 'class': 'btn cbi-button-positive', 'click': function(ev) {
						var values = {}, secrets = {}; ev.target.disabled = true;
						Object.keys(inputs).forEach(function(key) {
							var input = inputs[key];
							if (input.type === 'secret') { if (input.element.value) secrets[key] = input.element.value; }
							else values[key] = input.type === 'number' ? Number(input.element.value) : input.type === 'checkbox' ? (input.element.checked ? 1 : 0) : input.element.value;
						});
						saveChannel(row.id, name.value, kind.value, JSON.stringify(values), JSON.stringify(secrets), enabled.checked ? 1 : 0, row.version)
							.then(checked).then(function() { Object.keys(inputs).forEach(function(k) { if (inputs[k].type === 'secret') inputs[k].element.value = ''; }); ui.hideModal(); return refresh(); })
							.catch(report).finally(function() { ev.target.disabled = false; });
					} }, _('Save'))])]);
		}
		function refresh() {
			var serial = ++sequence;
			return Promise.all([listChannels().then(checked), listDevices().then(checked)]).then(function(result) {
				if (serial !== sequence) return null;
				channels = result[0].items || []; devices = result[1].items || [];
				if (!devices.some(function(d) { return d.id === selected; })) { selected = devices.length ? devices[0].id : 0; dirty = false; }
				deviceSelect.replaceChildren(); devices.forEach(function(d) { deviceSelect.appendChild(E('option', { 'value': String(d.id) }, d.name)); }); deviceSelect.value = String(selected);
				return selected ? Promise.all([bindings(selected).then(checked), pushHistory(selected).then(checked)]) : [{ items: [] }, { items: [] }];
			}).then(function(result) {
				if (!result || serial !== sequence) return;
				channelTable.replaceChildren(E('tr', { 'class': 'tr' }, [E('th', {}, _('Name')), E('th', {}, _('Type')), E('th', {}, _('Status')), E('th', {}, _('Actions'))]));
				channels.forEach(function(c) {
					var id = selected;
					channelTable.appendChild(E('tr', { 'class': 'tr' }, [E('td', {}, c.name), E('td', {}, names[c.kind] || c.kind),
						E('td', {}, (c.enabled ? _('Enabled') : _('Disabled')) + (c.kind === 'sms' ? '' : ' · ' + (c.has_credentials ? _('Credentials saved') : _('No saved credentials')))),
						E('td', {}, [E('button', { 'class': 'btn', 'disabled': (!writable) || null, 'click': function() { edit(c); } }, _('Edit')), ' ',
							E('button', { 'class': 'btn', 'disabled': (!writable || !selected || !c.enabled) || null, 'click': function() {
								var bytes = new Uint8Array(16); window.crypto.getRandomValues(bytes);
								var key = Array.from(bytes).map(function(n) { return n.toString(16).padStart(2, '0'); }).join('');
								confirm(_('Send test'), c.kind === 'sms' ? _('Send a test SMS to the forwarding number using the selected device SIM.') : _('Send a test message to this channel.'), function() { return testChannel(id, c.id, key); });
							} }, _('Test')), ' ', E('button', { 'class': 'btn', 'disabled': (!writable || !c.has_credentials) || null, 'click': function() {
								confirm(_('Clear credentials'), _('Clear this shared channel credentials, disable the channel and cancel pending forwards.'), function() { return clearCredentials(c.id, c.version); });
							} }, _('Clear credentials')), ' ', E('button', { 'class': 'btn cbi-button-remove', 'disabled': (!writable) || null, 'click': function() {
								confirm(_('Remove shared channel'), _('Stop using this channel on all linked devices and cancel pending forwards.'), function() { return removeChannel(c.id, c.version); });
							} }, _('Remove'))]) ]));
				});
				if (!dirty) {
					chosen = new Set((result[0].items || []).map(function(b) { return b.channel_id; })); bindingBox.replaceChildren();
					channels.forEach(function(c) {
						var box = E('input', { 'type': 'checkbox', 'checked': (chosen.has(c.id)) || null, 'disabled': (!writable) || null});
						box.addEventListener('change', function() { dirty = true; if (box.checked) chosen.add(c.id); else chosen.delete(c.id); });
						bindingBox.appendChild(E('p', {}, [box, ' ', c.name]));
					});
				}
				historyTable.replaceChildren(E('tr', { 'class': 'tr' }, [E('th', {}, _('Channel')), E('th', {}, _('Status')), E('th', {}, _('Attempts')), E('th', {}, _('Details'))]));
				(result[1].items || []).forEach(function(job) {
					historyTable.appendChild(E('tr', { 'class': 'tr' }, [E('td', {}, job.channel),
						E('td', {}, ({queued: _('Queued / waiting to retry'), attempting: _('Submitting'), waiting_send: _('Waiting for SIM send'), sent: _('Sent'), failed: _('Failed'), unconfirmed: _('Unconfirmed'), cancelled: _('Cancelled')})[job.status] || job.status),
						E('td', {}, String(job.attempts)), E('td', {}, (job.error ? _(job.error) : job.created_at))]));
				});
			});
		}
		deviceSelect.addEventListener('change', function() { selected = Number(deviceSelect.value); dirty = false; refresh().catch(report); });
		poll.add(function() { return refresh().catch(report); }, 5); refresh().catch(report);
		return E('div', {}, [E('h2', {}, _('Shared push channels')), E('button', { 'class': 'btn cbi-button-add', 'disabled': (!writable) || null, 'click': function() { edit(); } }, _('Add channel')),
			channelTable, E('h3', {}, _('Device bindings')), deviceSelect, E('p', {}, _('New bindings start from a history baseline. Existing progress resumes after restart. Failed forwards retry every minute.')), bindingBox,
			E('button', { 'class': 'btn cbi-button-positive', 'disabled': (!writable) || null, 'click': function() {
				if (selected) saveBindings(selected, JSON.stringify(Array.from(chosen))).then(checked).then(function() { dirty = false; return refresh(); }).catch(report);
			} }, _('Apply to selected device')), E('h3', {}, _('Pending and recent forwards (successful records cleared on restart)')), historyTable]);
	}
});
