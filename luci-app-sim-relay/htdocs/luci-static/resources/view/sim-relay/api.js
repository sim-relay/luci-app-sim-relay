'use strict';
'require view';
'require rpc';
'require ui';

var getSettings = rpc.declare({ object: 'sim-relay', method: 'api_settings' });
var saveSettings = rpc.declare({ object: 'sim-relay', method: 'save_api_settings',
	params: ['enabled', 'scope', 'port', 'transport', 'token'] });

function field(label, control, help) {
	return E('div', { 'class': 'cbi-value sim-relay-field' }, [
		E('label', { 'class': 'cbi-value-title' }, label),
		E('div', { 'class': 'cbi-value-field' }, help == null ? [control] : [control, help])
	]);
}
function report(error) { ui.addNotification(null, E('p', {}, _(error.message)), 'error'); }

return view.extend({
	handleSave: null, handleSaveApply: null, handleReset: null,
	load: function() { return getSettings(); },
	render: function(settings) {
		if (settings.error) throw new Error(settings.error);
		var writable = L.hasViewPermission() === true;
		var enabled = E('input', { 'type': 'checkbox', 'checked': settings.enabled ? true : null, 'disabled': !writable || null });
		var scope = E('select', { 'class': 'cbi-input-select', 'disabled': !writable || null }, [
			E('option', { 'value': 'lan' }, _('Local network only')),
			E('option', { 'value': 'any' }, _('All networks'))
		]);
		scope.value = settings.scope || 'lan';
		var transport = E('select', { 'class': 'cbi-input-select', 'disabled': !writable || null }, [
			E('option', { 'value': 'https' }, _('HTTPS (router certificate)')),
			E('option', { 'value': 'http' }, 'HTTP')
		]);
		transport.value = settings.transport || 'https';
		var port = E('input', { 'class': 'cbi-input-text', 'type': 'number', 'min': 1, 'max': 65535,
			'value': settings.port || 8848, 'disabled': !writable || null });
		var token = E('input', { 'class': 'cbi-input-text', 'type': 'text', 'autocomplete': 'off',
			'placeholder': settings.has_token ? _('Leave blank to keep the current token') : '', 'disabled': !writable || null });
		var generate = E('button', { 'class': 'btn', 'disabled': !writable || null, 'click': function() {
			var bytes = new Uint8Array(32); window.crypto.getRandomValues(bytes);
			token.value = Array.from(bytes).map(function(n) { return n.toString(16).padStart(2, '0'); }).join('');
		} }, _('Generate token'));
		var save = E('button', { 'class': 'btn cbi-button-positive', 'disabled': !writable || null, 'click': function(ev) {
			ev.target.disabled = true;
			saveSettings(enabled.checked ? 1 : 0, scope.value, Number(port.value), transport.value, token.value).then(function(result) {
				if (result.error) throw new Error(result.error);
				settings.has_token = settings.has_token || !!token.value;
				ui.addNotification(null, E('p', {}, _('API settings saved. Copy the token before leaving this page.')), 'info');
			}).catch(report).finally(function() { ev.target.disabled = false; });
		} }, _('Save'));
		return E('div', { 'class': 'sim-relay' }, [
			E('link', { 'rel': 'stylesheet', 'href': L.resource('sim-relay/style.css') }),
			E('h2', {}, _('Remote API')),
			E('div', { 'class': 'cbi-section' }, [E('div', { 'class': 'cbi-section-node' }, [
				field(_('Enable remote API'), enabled),
				field(_('Access scope'), scope, E('div', { 'class': 'sim-relay-help' }, _('All networks listens on all interfaces; firewall rules still control external access.'))),
				field(_('Transport'), transport, E('div', { 'class': 'sim-relay-help' }, _('HTTPS uses the router uhttpd certificate and SSL provider. HTTP sends the token without encryption.'))),
				field(_('Port'), port),
				field(_('API token'), token, E('div', { 'class': 'sim-relay-help' }, [generate, E('p', {}, _('The token is shown only here. Copy it before leaving; a new token replaces the old one.'))])),
				E('p', {}, _('API requests use a Bearer token. See the project API documentation for endpoints and examples.')),
				E('div', { 'class': 'sim-relay-actions' }, [save])
			])])
		]);
	}
});
