/* Exercise actual LuCI view event handlers with a simulated RPC boundary.
 * Minimal DOM/LuCI boundaries; this does not replace browser layout verification. */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '..');
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
let simICCID = "8986001234567890123", pinSaved = false;
const calls = [];
class Element {
    constructor(tag, attrs, children) {
        this.tag = tag; this.attrs = attrs || {}; this.style = {}; this.listeners = {}; this.children = [];
        this.value = this.attrs.value || ''; this.disabled = this.attrs.disabled != null; this.checked = this.attrs.checked != null;
        this.appendChild(children);
    }
    appendChild(child) {
        if (Array.isArray(child)) { child.forEach(c => this.appendChild(c)); return; }
        if (child == null) return;
        this.children.push(child);
        if (this.tag === 'select' && this.children.length === 1) this.value = child.value;
    }
    replaceChildren(...children) { this.children = []; if (this.tag === 'select') this.value = ''; children.forEach(c => this.appendChild(c)); }
    addEventListener(name, fn) { this.listeners[name] = fn; }
    get textContent() { return this._text == null ? this.children.map(c => c instanceof Element ? c.textContent : String(c)).join('') : this._text; }
    set textContent(value) { this._text = value; }
    emit(name) { const fn = this.listeners[name] || this.attrs[name]; if (fn) return fn({ target: this }); }
}
function E(tag, attrs, children) { return new Element(tag, attrs, children); }
function all(node, predicate) {
    if (!(node instanceof Element)) return [];
    return (predicate(node) ? [node] : []).concat(...node.children.map(c => all(c, predicate)));
}
function button(node, label) { const b = all(node, n => n.tag === 'button' && n.textContent === translate(label))[0]; assert.ok(b, label); assert.equal(b.disabled, false, label + ' must be clickable'); return b; }
function rpc(method, args = {}) {
    calls.push({ method, args });
    if (method === 'devices') return { items: savedDevice ? [savedDevice] : [] };
    if (method === 'sim_info') return { iccid: simICCID, saved: pinSaved, auto_enabled: 0,
        sim: { iccid: simICCID, unlock_required: 'sim-pin', pin_retries: 3, pin_helper_available: false } };
    if (method === 'save_pin') { pinSaved = true; return { ok: true }; }
    if (method === 'remove_device') { savedDevice = null; return { ok: true }; }
    if (method === 'channels') return { items: savedChannel ? [savedChannel] : [] };
    if (method === 'device_channels' || method === 'push_history') return { items: [] };
    if (method === 'save_channel') {
        if (channelSaveFails) return { error: 'Invalid channel settings' };
        savedChannel = { ...args, id: 7, config_json: args.config, has_credentials: 1 };
        return { id: 7 };
    }
    if (method === 'bind_channels') return { ok: true };
    if (method === 'messages' || method === 'send_history' || method === 'operation_history') return { items: [] };
    if (!mmAvailable) return { error: 'ModemManager is not installed. Install the modemmanager package before adding a local cellular modem.' };
    if (method === 'discover') return { items: [{ modem: '/org/freedesktop/ModemManager1/Modem/0', name: 'LTE', hardware_id: 'mm:123456789012345' }] };
    if (method === 'save_device') {
        savedDevice = { ...args, id: 1, hardware_id: 'mm:123456789012345' };
        return { id: 1 };
    }
    throw new Error('Unexpected RPC ' + method);
}
async function settle() { for (let i = 0; i < 8; i++) await new Promise(resolve => setImmediate(resolve)); }
async function main() {
    let modal = null; const notifications = [];
    const source = fs.readFileSync(path.join(root, 'luci-app-sim-relay/htdocs/luci-static/resources/view/sim-relay/overview.js'), 'utf8');
    const context = {
        E, _: translate, L: { hasViewPermission: () => viewPermission }, view: { extend: spec => spec }, poll: { add() {} },
        rpc: { declare: spec => (...values) => Promise.resolve(rpc(spec.method, Object.fromEntries((spec.params || []).map((p, i) => [p, values[i]])))) },
        ui: { showModal: (title, children) => { modal = E('div', {}, children); }, hideModal: () => { modal = null; },
            addNotification: (unused, message) => notifications.push(message.textContent) },
        Uint8Array, window: { crypto: require('node:crypto').webcrypto }, sessionStorage: { getItem: () => null, setItem() {}, removeItem() {} }
    };
    const view = vm.runInNewContext('(function(){' + source + '\n})()', context);
    const page = view.render(await view.load()); await settle();
    assert.ok(all(page, n => n.tag === 'button').some(n => n.textContent === (chinese ? '添加设备' : 'Add device')));
    button(page, 'Add device').emit('click');
    assert.equal(all(modal, n => n.tag === 'input' && n.attrs.type === 'checkbox')[0].checked, true, 'New devices default to enabled');
    assert.equal(all(modal, n => n.tag === 'select')[0].disabled, false, 'New device adapter must be selectable');
    const selects = all(modal, n => n.tag === 'select'), adapter = selects[0];
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
    assert.equal(modal, null);
    assert.equal(calls.filter(c => c.method === 'save_device').length, 2);
    // Editing preserves the same module and the new RPC argument order.
    button(page, 'Edit device').emit('click');
    const editName = all(modal, n => n.tag === 'input' && n.attrs.maxlength === 128)[0]; editName.value = 'renamed';
    button(modal, 'Save').emit('click'); await settle();
    assert.equal(rpc('devices').items[0].name, 'renamed');
    const simSource = fs.readFileSync(path.join(root, 'luci-app-sim-relay/htdocs/luci-static/resources/view/sim-relay/manage-pin.js'), 'utf8');
    const simView = vm.runInNewContext('(function(){' + simSource + '\n})()', context);
    const simPage = simView.render(await simView.load()); await settle();
    const savePIN = button(simPage, 'Save PIN settings');
    const checks = all(simPage, n => n.tag === 'input' && n.attrs.type === 'checkbox');
    assert.equal(checks[0].checked, false, 'Automatic unlock must default off');
    assert.equal(checks[1].checked, false, 'Force PIN attempt must default off');
    const pin = all(simPage, n => n.tag === 'input' && n.attrs.type === 'password')[0];
    pin.value = '1234'; pin.emit('input'); savePIN.emit('click'); await settle();
    const savedPIN = calls.find(c => c.method === 'save_pin');
    assert.equal(savedPIN.args.pin, '1234'); assert.equal(savedPIN.args.iccid, simICCID);
    assert.equal(savedPIN.args.auto_enabled, 0, 'Saving PIN must not silently enable automatic submission');
    assert.equal(pin.value, '', 'Clear PIN input after saving');
    simICCID = '';
    const missing = simView.render(await simView.load()); await settle();
    assert.equal(all(missing, n => n.tag === 'button' && n.textContent === translate('Save PIN settings'))[0].disabled, true);
    simICCID = '8986001234567890123';
    const readonlyView = vm.runInNewContext('(function(){' + simSource + '\n})()', { ...context, L: { hasViewPermission: () => false } });
    const readonlyPage = readonlyView.render(await readonlyView.load()); await settle();
    assert.equal(all(readonlyPage, n => n.tag === 'button' && n.textContent === translate('Save PIN settings'))[0].disabled, true);
    // Exercise channel configuration with the actual view and its RPC arguments.
    const channelsSource = fs.readFileSync(path.join(root, 'luci-app-sim-relay/htdocs/luci-static/resources/view/sim-relay/push-channels.js'), 'utf8');
    const channelsView = vm.runInNewContext('(function(){' + channelsSource + '\n})()', context);
    const channelsPage = channelsView.render(await channelsView.load()); await settle();
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
    const channelCheck = all(channelsPage, n => n.tag === 'input' && n.attrs.type === 'checkbox')[0];
    channelCheck.checked = true; channelCheck.emit('change');
    button(channelsPage, 'Apply to selected device').emit('click'); await settle();
    const bindingCall = calls.find(c => c.method === 'bind_channels');
    assert.equal(bindingCall.args.device_id, 1); assert.deepEqual(JSON.parse(bindingCall.args.ids), [7]);
    const readonlyChannels = vm.runInNewContext('(function(){' + channelsSource + '\n})()', { ...context, L: { hasViewPermission: () => false } });
    const readonlyChannelsPage = readonlyChannels.render(await readonlyChannels.load()); await settle();
    assert.equal(all(readonlyChannelsPage, n => n.tag === 'button' && n.textContent === translate('Add channel'))[0].disabled, true);
    assert.equal(all(readonlyChannelsPage, n => n.tag === 'button' && n.textContent === translate('Edit'))[0].disabled, true);
    button(page, 'Remove device').emit('click');
    assert.ok(savedDevice, 'Opening the confirmation must not remove the device');
    assert.match(modal.textContent, chinese ? /SIM PIN/ : /saved SIM PIN/);
    button(modal, 'Remove device').emit('click'); await settle();
    assert.equal(savedDevice, null, 'Confirmed removal should call the backend');
    assert.equal(calls.find(c => c.method === 'remove_device').args.device_id, 1);
    console.log((chinese ? 'Chinese' : 'English') + ' LuCI device, PIN and channel configuration flows passed.');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
