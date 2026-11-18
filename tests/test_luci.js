/* Exercise actual LuCI view event handlers with a simulated RPC boundary.
 * Minimal DOM/LuCI boundaries; this does not replace browser layout verification. */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '..');
const viewRoot = process.env.SIM_RELAY_LUCI_VIEW_DIR || path.join(root, 'luci-app-sim-relay/htdocs/luci-static/resources/view/sim-relay');
const menu = JSON.parse(fs.readFileSync(path.join(root, 'luci-app-sim-relay/root/usr/share/luci/menu.d/luci-app-sim-relay.json'), 'utf8'));
for (const node of Object.values(menu)) {
    if (node.action.type === 'view') assert.ok(node.depends?.acl?.includes('luci-app-sim-relay'), 'Each view must declare its ACL');
}
const viewPermission = menu['admin/services/sim-relay/overview'].depends?.acl?.includes('luci-app-sim-relay') ? true : null;
const chinese = process.argv.includes('--zh-cn');
const translations = {};
if (chinese) {
    let key;
    for (const line of fs.readFileSync(path.join(root, 'luci-app-sim-relay/po/zh_Hans/sim-relay.po'), 'utf8').split('\n')) {
        if (line.startsWith('msgid ')) key = JSON.parse(line.slice(6));
        else if (line.startsWith('msgstr ') && key) translations[key] = JSON.parse(line.slice(7));
    }
}
function translate(text) { return translations[text] || text; }
let mmAvailable = false;
let savedDevice = null;
let savedChannel = null;
let channelSaveFails = false;
let simICCID = "8986001234567890123", pinSaved = false, autoUnlock = 0;
let deviceMessages = [{ id: 1, number: '10086', text: 'hello', timestamp: '2026-11-17T08:00:00Z', direction: 'inbound' }];
let boundChannels = new Set();
let extraChannels = [];
let pushHistoryItems = [];
let apiSettings = { enabled: 0, scope: 'lan', transport: 'https', port: 8848, has_token: false };
const calls = [];
class Element {
    constructor(tag, attrs, children) {
        this.tag = tag; this.attrs = attrs || {}; this.style = {}; this.listeners = {}; this.children = [];
        this.value = this.attrs.value || ''; this.disabled = this.attrs.disabled != null; this.checked = this.attrs.checked != null;
        this.appendChild(children);
    }
    appendChild(child) {
        // LuCI dom.append() stringifies null and undefined inside child arrays.
        if (Array.isArray(child)) { child.forEach(c => this.appendChild(c == null ? String(c) : c)); return; }
        if (child == null) return;
        this.children.push(child);
        if (this.tag === 'select' && this.children.length === 1) this.value = child.value;
    }
    replaceChildren(...children) { this.children = []; if (this.tag === 'select') this.value = ''; children.forEach(c => this.appendChild(c)); }
    addEventListener(name, fn) { this.listeners[name] = fn; }
    get textContent() { return this._text == null ? this.children.map(c => c instanceof Element ? c.textContent : String(c)).join('') : this._text; }
    set textContent(value) { this._text = value; }
    emit(name) { const fn = this.listeners[name] || this.attrs[name]; if (fn) return fn({ target: this }); }
    querySelector() { return null; }
}
function E(tag, attrs, children) { return new Element(tag, attrs, children); }
class Dropdown {
    constructor(value, choices, options) { this.value = value; this.choices = choices; this.options = options; }
    render() {
        this.node = E('div', { class: 'cbi-dropdown', disabled: this.options.disabled ? '' : null });
        this.node.widget = this;
        return this.node;
    }
    getValue() { return this.value; }
    setValue(value) { this.value = value; this.node.emit('cbi-dropdown-change'); }
}
function all(node, predicate) {
    if (!(node instanceof Element)) return [];
    return (predicate(node) ? [node] : []).concat(...node.children.map(c => all(c, predicate)));
}
function assertNoMissingText(node) {
    for (const el of all(node, () => true)) {
        if (typeof el._text === 'string') assert.doesNotMatch(el._text, /\b(?:undefined|null)\b/, 'Missing value rendered as text');
        for (const child of el.children) {
            if (typeof child === 'string') assert.doesNotMatch(child, /\b(?:undefined|null)\b/, 'Missing value rendered as text');
        }
    }
}
function button(node, label) { const b = all(node, n => n.tag === 'button' && n.textContent === translate(label))[0]; assert.ok(b, label); assert.equal(b.disabled, false, label + ' must be clickable'); return b; }
function rpc(method, args = {}) {
    calls.push({ method, args });
    if (method === 'devices') return { items: savedDevice ? [savedDevice] : [] };
    if (method === 'sim_info') return { iccid: simICCID, saved: pinSaved, auto_enabled: autoUnlock,
        sim: { iccid: simICCID, unlock_required: 'sim-pin', pin_retries: 3, pin_helper_available: false } };
    if (method === 'save_pin') {
        if (args.forget) pinSaved = false;
        else if (args.pin) pinSaved = true;
        if (args.auto_enabled != null) autoUnlock = args.auto_enabled;
        return { ok: true };
    }
    if (method === 'unlock_pin') return { ok: true };
    if (method === 'remove_device') { savedDevice = null; return { ok: true }; }
    if (method === 'channels') return { items: (savedChannel ? [savedChannel] : []).concat(extraChannels) };
    if (method === 'device_channels') return { items: Array.from(boundChannels).map(id => ({ channel_id: id })) };
    if (method === 'push_history') return { items: pushHistoryItems };
    if (method === 'save_channel') {
        if (channelSaveFails) return { error: 'Invalid channel settings' };
        savedChannel = { ...args, id: 7, config_json: args.config, has_credentials: 1 };
        return { id: 7 };
    }
    if (method === 'bind_channels') { boundChannels = new Set(JSON.parse(args.ids)); return { ok: true }; }
    if (method === 'messages') return { items: deviceMessages };
    if (method === 'api_settings') return apiSettings;
    if (method === 'save_api_settings') { apiSettings = { ...args, has_token: !!args.token }; return { ok: true }; }
    if (method === 'delete_messages') return { id: 9 };
    if (method === 'sync' || method === 'send' || method === 'test_channel') return { ok: true, id: 9 };
    if (!mmAvailable) return { error: 'ModemManager is not installed. Install the modemmanager package before adding a local cellular modem.' };
    if (method === 'discover') return { items: [{ modem: '/org/freedesktop/ModemManager1/Modem/0', name: 'LTE', hardware_id: 'mm:123456789012345' }] };
    if (method === 'save_device') {
        savedDevice = { ...args, id: 1, hardware_id: 'mm:123456789012345' };
        return { id: 1 };
    }
    throw new Error('Unexpected RPC ' + method);
}
async function settle() { for (let i = 0; i < 20; i++) await new Promise(resolve => setImmediate(resolve)); }
async function main() {
    let modal = null; const notifications = [];
    const context = {
        E, _: translate, L: { hasViewPermission: () => viewPermission, resource: name => '/luci-static/resources/' + name }, view: { extend: spec => spec }, poll: { add() {} },
        rpc: { declare: spec => (...values) => Promise.resolve(rpc(spec.method, Object.fromEntries((spec.params || []).map((p, i) => [p, values[i]])))) },
        ui: { Dropdown, showModal: (title, children) => { modal = E('div', {}, children); assertNoMissingText(modal); }, hideModal: () => { modal = null; },
            addNotification: (unused, message) => notifications.push(message.textContent) },
        Uint8Array, window: { crypto: require('node:crypto').webcrypto }, sessionStorage: { getItem: () => null, setItem() {}, removeItem() {} }
    };
    const manageSource = fs.readFileSync(path.join(viewRoot, 'manage-pin.js'), 'utf8');
    const manageView = vm.runInNewContext('(function(){' + manageSource + '\n})()', context);
    const managePage = manageView.render(await manageView.load()); await settle();
    assertNoMissingText(managePage);
    button(managePage, 'Add device').emit('click');
    assert.equal(all(modal, n => n.tag === 'input' && n.attrs.type === 'checkbox')[0].checked, true, 'New devices default to enabled');
    assert.equal(all(modal, n => n.tag === 'select')[0].disabled, false, 'New device adapter must be selectable');
    const selects = all(modal, n => n.tag === 'select'), adapter = selects[0];
    for (const type of ['zte_mf833u1', 'mock', 'modemmanager']) {
        adapter.value = type; adapter.emit('change'); await settle(); assertNoMissingText(modal);
    }
    adapter.value = 'modemmanager'; adapter.emit('change'); await settle();
    assert.match(modal.textContent, chinese ? /安装 modemmanager/ : /Install the modemmanager package/);
    const name = all(modal, n => n.tag === 'input' && n.attrs.maxlength === 128)[0]; name.value = 'local';
    button(modal, 'Save').emit('click'); await settle();
    assert.match(notifications.at(-1), chinese ? /安装 modemmanager/ : /Install the modemmanager package/);
    assert.equal(rpc('devices').items.length, 0);
    // No automatic installation. Once available, scan and add the selected module.
    mmAvailable = true;
    adapter.emit('change'); await settle();
    assert.equal(selects[1].value, '/org/freedesktop/ModemManager1/Modem/0');
    button(modal, 'Save').emit('click'); await settle();
    const device = rpc('devices').items[0];
    assert.equal(device.adapter, 'modemmanager'); assert.equal(device.hardware_id, 'mm:123456789012345');
    assert.equal(device.poll_interval, 30); assert.equal(device.enabled, 1); assert.equal(device.base_url, '');
    assert.equal(device.keep_messages, 0, 'Retention starts disabled');
    assert.equal(modal, null);
    assert.equal(calls.filter(c => c.method === 'save_device').length, 2);
    // Editing preserves the same module and stores the retention limit.
    button(managePage, 'Edit').emit('click');
    const editName = all(modal, n => n.tag === 'input' && n.attrs.maxlength === 128)[0]; editName.value = 'renamed';
    const keep = all(modal, n => n.tag === 'input' && n.attrs.type === 'number' && n.attrs.min === 0)[0];
    keep.value = '2';
    button(modal, 'Save').emit('click'); await settle();
    assert.equal(rpc('devices').items[0].name, 'renamed');
    assert.equal(rpc('devices').items[0].keep_messages, 2);
    // SIM rows are read from the device and expose PIN, auto-unlock and manual unlock.
    const setPIN = button(managePage, 'Set PIN');
    setPIN.emit('click');
    const pin = all(modal, n => n.tag === 'input' && n.attrs.type === 'password')[0];
    pin.value = '1234'; pin.emit('input');
    button(modal, 'Save').emit('click'); await settle();
    const savedPIN = calls.find(c => c.method === 'save_pin');
    assert.equal(savedPIN.args.pin, '1234'); assert.equal(savedPIN.args.iccid, simICCID);
    assert.equal(savedPIN.args.auto_enabled, 0, 'Saving PIN must not silently enable automatic submission');
    const automatic = all(managePage, n => n.tag === 'input' && n.attrs.type === 'checkbox')[0];
    assert.equal(automatic.disabled, false, 'Auto unlock becomes available after a PIN is saved');
    assert.equal(automatic.checked, false);
    automatic.checked = true; automatic.emit('change'); await settle();
    const autoCall = calls.filter(c => c.method === 'save_pin').at(-1);
    assert.equal(autoCall.args.pin, ''); assert.equal(autoCall.args.auto_enabled, 1);
    assert.equal(all(managePage, n => n.tag === 'input' && n.attrs.type === 'checkbox')[0].checked, true, 'Auto unlock reflects the saved setting');
    button(managePage, 'Unlock once').emit('click');
    assert.equal(all(modal, n => n.tag === 'input' && n.attrs.type === 'checkbox')[0].checked, false, 'Force PIN attempt must default off');
    button(modal, 'Unlock once').emit('click'); await settle();
    const unlockCall = calls.find(c => c.method === 'unlock_pin');
    assert.equal(unlockCall.args.device_id, 1); assert.equal(unlockCall.args.iccid, simICCID); assert.equal(unlockCall.args.force, 0);
    simICCID = '';
    const missing = manageView.render(await manageView.load()); await settle();
    assert.equal(all(missing, n => n.tag === 'button' && n.textContent === translate('Set PIN'))[0].disabled, true);
    simICCID = '8986001234567890123';
    const readonlyView = vm.runInNewContext('(function(){' + manageSource + '\n})()', { ...context, L: { hasViewPermission: () => false, resource: name => '/luci-static/resources/' + name } });
    const readonlyPage = readonlyView.render(await readonlyView.load()); await settle();
    assert.equal(all(readonlyPage, n => n.tag === 'button' && n.textContent === translate('Add device'))[0].disabled, true);
    assert.equal(all(readonlyPage, n => n.tag === 'button' && n.textContent === translate('Set PIN'))[0].disabled, true);
    // SMS page: device selector, selectable messages, delete and send only.
    const overviewSource = fs.readFileSync(path.join(viewRoot, 'overview.js'), 'utf8');
    const overviewView = vm.runInNewContext('(function(){' + overviewSource + '\n})()', context);
    const page = overviewView.render(await overviewView.load()); await settle();
    assertNoMissingText(page);
    const searchField = all(page, n => n.tag === 'div' && n.attrs.class === 'cbi-value sim-relay-field' &&
        all(n, child => child.tag === 'input' && child.attrs.type === 'search').length === 1)[0];
    assert.ok(searchField, 'The SMS search field must be present');
    const searchControls = all(searchField, n => n.tag === 'div' && n.attrs.class === 'cbi-value-field')[0];
    assert.equal(searchControls.children.length, 1, 'An absent search description must not render as text');
    assert.equal(searchControls.children[0].attrs.placeholder, translate('Search sender or text'));
    assert.equal(all(page, n => n.tag === 'button' && n.textContent === translate('Add device')).length, 0, 'SMS page must not manage devices');
    assert.ok(button(page, 'Send SMS'));
    const boxes = all(page, n => n.tag === 'input' && n.attrs.type === 'checkbox');
    boxes[1].checked = true; boxes[1].emit('change');
    button(page, 'Delete selected messages').emit('click');
    assert.match(modal.textContent, chinese ? /不可撤销/ : /cannot be undone/);
    button(modal, 'Delete selected messages').emit('click'); await settle();
    const deleteCall = calls.find(c => c.method === 'delete_messages');
    assert.equal(deleteCall.args.device_id, 1); assert.deepEqual(JSON.parse(deleteCall.args.ids), [1]); assert.equal(deleteCall.args.scope, 'device');
    assert.ok(calls.some(c => c.method === 'sync'), 'Deleting should request a device refresh');
    button(page, 'Send SMS').emit('click');
    const tel = all(modal, n => n.tag === 'input' && n.attrs.type === 'tel')[0]; tel.value = '10086';
    const textarea = all(modal, n => n.tag === 'textarea')[0]; textarea.value = 'hello';
    button(modal, 'Send').emit('click'); await settle();
    const sendCall = calls.find(c => c.method === 'send');
    assert.equal(sendCall.args.device_id, 1); assert.equal(sendCall.args.number, '10086'); assert.equal(sendCall.args.text, 'hello');
    // Exercise channel configuration with the actual view and its RPC arguments.
    const channelsSource = fs.readFileSync(path.join(viewRoot, 'push-channels.js'), 'utf8');
    const channelsView = vm.runInNewContext('(function(){' + channelsSource + '\n})()', context);
    const channelsPage = channelsView.render(await channelsView.load()); await settle();
    assertNoMissingText(channelsPage);
    // Every new/edit channel form uses the same optional-help renderer.
    button(channelsPage, 'Add channel').emit('click');
    const allKinds = all(modal, n => n.tag === 'select')[0];
    for (const type of ['sms', 'telegram', 'wecom', 'feishu', 'smtp', 'webhook']) {
        allKinds.value = type; allKinds.emit('change'); assertNoMissingText(modal);
    }
    context.ui.hideModal();
    const previousChannel = savedChannel;
    for (const type of ['sms', 'telegram', 'wecom', 'feishu', 'smtp', 'webhook']) {
        savedChannel = { id: 7, name: 'existing', kind: type, enabled: 1, version: 1, config_json: '{}', has_credentials: 1 };
        const editPage = channelsView.render(await channelsView.load()); await settle();
        assertNoMissingText(editPage);
        button(editPage, 'Edit').emit('click'); assertNoMissingText(modal);
        context.ui.hideModal();
    }
    savedChannel = previousChannel;
    button(channelsPage, 'Add channel').emit('click');
    const channelName = all(modal, n => n.tag === 'input' && n.attrs.maxlength === 128)[0]; channelName.value = 'test';
    const secret = all(modal, n => n.tag === 'input' && n.attrs.type === 'password')[0]; secret.value = '123456789:example-token';
    const textFields = all(modal, n => n.tag === 'input' && n.attrs.type === 'text'); textFields[0].value = '42';
    channelSaveFails = true;
    button(modal, 'Save').emit('click'); await settle();
    assert.match(notifications.at(-1), /Invalid channel settings/);
    button(modal, 'Save'); // A rejected save must not leave the button disabled.
    channelSaveFails = false;
    button(modal, 'Save').emit('click'); await settle();
    assert.equal(modal, null, 'Channel save should close the form');
    const channelCall = calls.find(c => c.method === 'save_channel');
    assert.equal(channelCall.args.name, 'test'); assert.equal(channelCall.args.kind, 'telegram');
    assert.equal(JSON.parse(channelCall.args.config).chat_id, '42');
    assert.equal(JSON.parse(channelCall.args.secrets).token, '123456789:example-token');
    assert.equal(secret.value, '', 'Clear channel credentials after saving');
    button(channelsPage, 'Edit').emit('click');
    assert.equal(all(modal, n => n.tag === 'select')[0].disabled, true, 'Existing channel type cannot be changed');
    context.ui.hideModal();
    const selector = all(channelsPage, n => n.widget instanceof Dropdown)[0];
    assert.equal(selector.widget.options.multiple, true, 'Bindings must allow multiple channels');
    selector.widget.setValue(['7']);
    button(channelsPage, 'Save').emit('click'); await settle();
    const bindingCall = calls.find(c => c.method === 'bind_channels');
    assert.equal(bindingCall.args.device_id, 1); assert.deepEqual(JSON.parse(bindingCall.args.ids), [7]);
    extraChannels = [{ id: 8, name: 'second', kind: 'sms', enabled: 0 }];
    const multiPage = channelsView.render(await channelsView.load()); await settle();
    const multiSelector = all(multiPage, n => n.widget instanceof Dropdown)[0];
    assert.deepEqual(Array.from(multiSelector.widget.value), ['7'], 'Saved bindings remain selected');
    assert.ok(multiSelector.widget.choices['8'].includes(translate('Disabled')));
    multiSelector.widget.setValue(['7', '8']);
    button(multiPage, 'Save').emit('click'); await settle();
    assert.deepEqual(JSON.parse(calls.filter(c => c.method === 'bind_channels').at(-1).args.ids), [7, 8]);
    const emptySelector = all(multiPage, n => n.widget instanceof Dropdown)[0];
    emptySelector.widget.setValue([]);
    button(multiPage, 'Save').emit('click'); await settle();
    assert.deepEqual(JSON.parse(calls.filter(c => c.method === 'bind_channels').at(-1).args.ids), [], 'All bindings can be cleared');
    const readonlyChannels = vm.runInNewContext('(function(){' + channelsSource + '\n})()', { ...context, L: { hasViewPermission: () => false, resource: name => '/luci-static/resources/' + name } });
    const readonlyChannelsPage = readonlyChannels.render(await readonlyChannels.load()); await settle();
    assert.equal(all(readonlyChannelsPage, n => n.tag === 'button' && n.textContent === translate('Add channel'))[0].disabled, true);
    assert.equal(all(readonlyChannelsPage, n => n.tag === 'button' && n.textContent === translate('Edit'))[0].disabled, true);
    assert.equal(all(readonlyChannelsPage, n => n.widget instanceof Dropdown)[0].widget.options.disabled, true);
    button(channelsPage, 'Add channel').emit('click');
    const kind = all(modal, n => n.tag === 'select')[0]; kind.value = 'webhook'; kind.emit('change');
    assertNoMissingText(modal);
    all(modal, n => n.tag === 'input' && n.attrs.maxlength === 128)[0].value = 'custom';
    all(modal, n => n.tag === 'input' && n.attrs.type === 'password')[0].value = 'http://example.test/?m={{message}}';
    const requestMethod = all(modal, n => n.tag === 'select')[1];
    const bodyRow = all(modal, n => n.attrs.class === 'cbi-value sim-relay-field' && n.textContent.includes(translate('Request body')))[0];
    requestMethod.value = 'GET'; requestMethod.emit('change');
    assert.equal(bodyRow.style.display, 'none', 'GET does not display a request body');
    requestMethod.value = 'PUT'; requestMethod.emit('change');
    assert.equal(bodyRow.style.display, '', 'PUT displays the request body');
    const requestAreas = all(modal, n => n.tag === 'textarea');
    requestAreas[0].value = 'Content-Type: application/json\nAuthorization: Bearer secret';
    requestAreas[1].value = '{"text":{{message_json}}}';
    assert.ok(modal.textContent.includes('{{message_json}}'), 'Explain the JSON placeholder');
    button(modal, 'Save').emit('click'); await settle();
    const webhookCall = calls.filter(c => c.method === 'save_channel').at(-1);
    assert.equal(webhookCall.args.kind, 'webhook');
    assert.equal(JSON.parse(webhookCall.args.config).method, 'PUT');
    assert.equal(JSON.parse(webhookCall.args.config).body, '{"text":{{message_json}}}');
    assert.match(JSON.parse(webhookCall.args.secrets).headers, /Bearer secret/);
    assert.match(JSON.parse(webhookCall.args.secrets).url, /{{message}}/);
    button(channelsPage, 'Edit').emit('click');
    const clearHeaders = all(modal, n => n.tag === 'input' && n.attrs.type === 'checkbox')[0];
    clearHeaders.checked = true;
    button(modal, 'Save').emit('click'); await settle();
    const clearCall = calls.filter(c => c.method === 'save_channel').at(-1);
    assert.equal(JSON.parse(clearCall.args.secrets).headers, '');
    assert.equal(JSON.parse(clearCall.args.secrets).url, undefined, 'Blank URL keeps the stored secret');
    const apiSource = fs.readFileSync(path.join(viewRoot, 'api.js'), 'utf8');
    const apiView = vm.runInNewContext('(function(){' + apiSource + '\n})()', context);
    const apiPage = apiView.render(await apiView.load()); assertNoMissingText(apiPage);
    const apiEnabled = all(apiPage, n => n.tag === 'input' && n.attrs.type === 'checkbox')[0];
    assert.equal(apiEnabled.checked, false, 'Remote API defaults to disabled');
    const apiSelects = all(apiPage, n => n.tag === 'select');
    assert.equal(apiSelects[0].value, 'lan'); assert.equal(apiSelects[1].value, 'https');
    button(apiPage, 'Generate token').emit('click');
    const apiToken = all(apiPage, n => n.tag === 'input' && n.attrs.type === 'text')[0];
    assert.match(apiToken.value, /^[0-9a-f]{64}$/);
    apiEnabled.checked = true; apiSelects[0].value = 'any';
    button(apiPage, 'Save').emit('click'); await settle();
    const apiCall = calls.filter(c => c.method === 'save_api_settings').at(-1);
    assert.equal(apiCall.args.enabled, 1); assert.equal(apiCall.args.scope, 'any');
    assert.equal(apiCall.args.transport, 'https'); assert.equal(apiCall.args.token, apiToken.value);
    // Older device records and new pending jobs may omit optional fields.
    const originalDevice = savedDevice;
    savedDevice = { ...savedDevice }; delete savedDevice.poll_interval; delete savedDevice.adapter;
    pushHistoryItems = [{ channel: 'custom', status: 'queued' }];
    const originalMessages = deviceMessages;
    deviceMessages = [{ id: 1, direction: 'inbound' }];
    const sparseManage = manageView.render(await manageView.load());
    const sparseOverview = overviewView.render(await overviewView.load());
    const sparseChannels = channelsView.render(await channelsView.load()); await settle();
    [sparseManage, sparseOverview, sparseChannels].forEach(assertNoMissingText);
    assert.ok(sparseChannels.textContent.includes('0'), 'A newly queued job shows zero attempts');
    savedDevice = originalDevice; pushHistoryItems = []; deviceMessages = originalMessages;
    button(managePage, 'Remove').emit('click');
    assert.ok(savedDevice, 'Opening the confirmation must not remove the device');
    assert.match(modal.textContent, chinese ? /SIM PIN/ : /saved SIM PIN/);
    button(modal, 'Remove device').emit('click'); await settle();
    assert.equal(savedDevice, null, 'Confirmed removal should call the backend');
    assert.equal(calls.find(c => c.method === 'remove_device').args.device_id, 1);
    const emptyManage = manageView.render(await manageView.load());
    const emptyOverview = overviewView.render(await overviewView.load());
    const emptyChannels = channelsView.render(await channelsView.load()); await settle();
    [emptyManage, emptyOverview, emptyChannels].forEach(assertNoMissingText);
    console.log((chinese ? 'Chinese' : 'English') + ' LuCI device, PIN and channel configuration flows passed.');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
