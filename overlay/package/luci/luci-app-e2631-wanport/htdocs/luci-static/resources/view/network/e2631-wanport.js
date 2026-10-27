'use strict';
'require view';
'require dom';
'require form';
'require rpc';
'require poll';
'require ui';

/*
 * WAN port: which wired port carries the uplink.
 *
 * The vendor calls this "WAN/LAN 自适应" and drives a switch-chip ioctl from
 * cspd (see docs/wanlan-adaptive-逆向记录-2026-10-27.md). This board has four
 * separate GMAC netdevs, so the same thing is pure userspace: rebind
 * network.wan.device to the chosen port and take that port out of br-lan.
 * /sbin/e2631-wanport does the work; this page only reads its state and asks
 * it to run.
 *
 * There is deliberately no automatic mode: the router never decides on its own
 * which port is the uplink. Two things came out of the hardware run on
 * 2026-10-27: a DHCP probe cannot see a PPPoE/static uplink at all, and an
 * automatic switch can move the WAN onto the very port this browser is coming
 * in on (WAN zone input is REJECT, so the box then answers ping and nothing
 * else). Default layout = the factory one: WAN on the "wan" port, LAN1..3 in
 * the bridge.
 */

var callStatus = rpc.declare({
	object: 'e2631-wanport',
	method: 'status',
	expect: { '': {} }
});

var callSetPort = rpc.declare({
	object: 'e2631-wanport',
	method: 'set_port',
	params: [ 'port' ],
	expect: { '': {} }
});

/* LuCI's themes style ".label.success" / ".label.warning"; there is no
 * ".label.danger", so "off/down" is a warning, not an error. */
function badge(text, cls) {
	return E('span', { 'class': 'label' + (cls ? ' ' + cls : '') }, text);
}

function linkText(p) {
	if (p.carrier !== true)
		return _('no cable');
	return p.speed && p.speed !== '-'
		? _('connected, %s Mb/s').format(p.speed)
		: _('connected');
}

return view.extend({
	load: function() {
		return callStatus();
	},

	renderPorts: function(node, s) {
		var self = this;
		var rows = [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, _('Port')),
				E('th', { 'class': 'th' }, _('Role')),
				E('th', { 'class': 'th' }, _('Link')),
				E('th', { 'class': 'th' }, _('Bridge')),
				E('th', { 'class': 'th' }, _('MAC address')),
				E('th', { 'class': 'th' }, '')
			])
		];

		var ports = s.ports || [];
		if (!ports.length) {
			rows.push(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td', 'colspan': 6 },
					E('em', _('No ports found: /sbin/e2631-wanport is not installed on this router.')))
			]));
		}

		ports.forEach(function(p) {
			var action = '';
			if (p.role !== 'wan' && s.enabled !== '0') {
				action = E('button', {
					'class': 'btn cbi-button cbi-button-action',
					'click': ui.createHandlerFn(self, function() {
						return callSetPort(p.name).then(function(res) {
							if (res && res.ok)
								ui.addNotification(null, E('p',
									_('Switching the WAN to %s. The uplink drops for a few seconds while the bridge and the WAN interface are moved.').format(p.label)), 'info');
							else
								ui.addNotification(null, E('p',
									_('The switch could not be started: %s').format(res && res.error ? res.error : '?')), 'danger');
						});
					})
				}, _('Use as WAN'));
			}

			rows.push(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, [ E('strong', {}, p.label) ]),
				E('td', { 'class': 'td' }, [
					p.role === 'wan' ? badge(_('WAN'), 'success') : badge(_('LAN'))
				]),
				E('td', { 'class': 'td' }, linkText(p)),
				E('td', { 'class': 'td' }, p.master ? p.master : E('em', _('not in a bridge'))),
				E('td', { 'class': 'td' }, E('code', {}, p.mac || '-')),
				E('td', { 'class': 'td cbi-section-actions' }, action)
			]));
		});

		dom.content(node, [
			E('table', { 'class': 'table' }, rows),
			E('p', { 'class': 'cbi-section-descr' },
				_('LAN and WAN are roles here, not hardware: the WAN port is whichever port the WAN interface is bound to and which is out of the LAN bridge. Nothing is detected or switched automatically — a port becomes the WAN only when you say so. The WAN MAC stays the same across a port change, so the uplink lease and the IPv6 identity of this router survive it.'))
		]);
	},

	renderStatus: function(node, s) {
		var state = [];
		state.push(E('p', {}, [
			_('WAN'), ': ', E('strong', {}, s.device || '-'), ' — ',
			s.wan_up ? badge(_('up'), 'success') : badge(_('down'), 'warning'),
			'   ', _('configured port'), ': ', E('code', {}, s.manual_port || '-')
		]));
		state.push(E('p', {}, [
			_('WAN MAC'), ': ', E('code', {}, s.wan_mac || '-'), ' ',
			_('Bridge'), ': ', E('code', {}, s.bridge || '-'), ' ',
			_('members'), ': ', E('code', {}, s.bridge_ports || '-')
		]));
		if (s.enabled === '0')
			state.push(E('p', {}, E('em', _('Changing the WAN port is switched off (enabled = 0). The current layout stays as it is.'))));

		dom.content(node, state);
	},

	render: function(s) {
		var self = this;

		var statusBox = E('div', { 'class': 'cbi-section', 'id': 'e2631-wanport-status' });
		var portsBox = E('div', { 'class': 'cbi-section', 'id': 'e2631-wanport-ports' });

		this.renderStatus(statusBox, s);
		this.renderPorts(portsBox, s);

		poll.add(function() {
			return callStatus().then(function(res) {
				dom.content(statusBox, []);
				self.renderStatus(statusBox, res);
				self.renderPorts(portsBox, res);
			});
		}, 5);

		var m = new form.Map('e2631_ports', _('WAN port'),
			_('Any of the four wired ports can carry the uplink. This is the same feature the factory calls "WAN/LAN adaptive", implemented without touching the switch: a port is a WAN port when the WAN interface is bound to it and it is out of the LAN bridge. The default is the factory layout — the "wan" port is the WAN, LAN1..3 are the switch.'));

		var s2 = m.section(form.NamedSection, 'global', 'ports');
		s2.addremove = false;
		var o;

		o = s2.option(form.Flag, 'enabled', _('Allow changing the WAN port'));
		o.default = '1';
		o.rmempty = false;
		o.description = _('Off: the router keeps its current layout and the buttons on this page are disabled.');

		o = s2.option(form.ListValue, 'manual_port', _('WAN port'));
		(s.ports || []).forEach(function(p) {
			o.value(p.name, p.label);
		});
		o.default = 'wan';
		o.rmempty = false;
		o.description = _('The port that should carry the uplink. Saving applies it: the port leaves the LAN bridge, the WAN interface is bound to it, and the other three ports join the bridge. The uplink drops for a few seconds while that happens.');

		return m.render().then(function(formNode) {
			var head = E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Current state')), statusBox
			]);
			var ports = E('div', { 'class': 'cbi-section' }, [ E('h3', _('Ports')), portsBox ]);
			return E([ head, ports, formNode ]);
		});
	},

	/* Saving the form only writes uci; the port move is a service action, so run
	 * it right after the save and let it report through the status poll. */
	handleSaveApply: function(ev, mode) {
		return this.handleSave(ev).then(function() {
			return callStatus();
		}).then(function(res) {
			var port = res && res.manual_port ? res.manual_port : null;
			if (port)
				return callSetPort(port);
		}).then(function() {
			ui.changes.apply(mode == '0');
		});
	}
});
