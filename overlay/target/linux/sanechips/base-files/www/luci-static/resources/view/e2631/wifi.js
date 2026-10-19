'use strict';
'require view';
'require fs';
'require ui';

var command = '/sbin/e2631-wifi-init';

function statusText(value) {
    switch (value) {
    case 'DRIVER_READY':
        return '驱动已就绪，可以进入无线设置';
    case 'INITIALIZING':
        return 'MT7916 正在初始化';
    case 'DRIVER_OFF':
        return '无线驱动尚未加载';
    default:
        return '状态未知，请检查后端程序';
    }
}

function updateStatus(node) {
    return fs.exec(command, ['status']).then(function(res) {
        if (res.code !== 0)
            throw new Error(res.stderr || '状态查询失败');

        node.textContent = statusText((res.stdout || '').trim());
    }).catch(function(err) {
        node.textContent = '查询失败：' + err.message;
    });
}

return view.extend({
    load: function() {
        return fs.exec(command, ['status']).catch(function() {
            return { code: -1, stdout: '' };
        });
    },

    render: function(res) {
        var state = E('p', {
            'class': 'e2631-wifi-state'
        }, statusText((res.stdout || '').trim()));

        return E('div', { 'class': 'cbi-map' }, [
            E('h2', {}, 'E2631 MT7916 无线初始化'),

            E('p', {}, '初始化无线驱动后，可以通过 LuCI 正常配置 Wi-Fi。'),

            state,

            E('div', {}, [
                E('button', {
                    'type': 'button',
                    'class': 'btn cbi-button cbi-button-action',
                    'click': ui.createHandlerFn(this, 'handleStart', state)
                }, '初始化无线驱动'),

                ' ',

                E('button', {
                    'type': 'button',
                    'class': 'btn cbi-button',
                    'click': ui.createHandlerFn(this, 'handleRefresh', state)
                }, '刷新状态')
            ]),

            E('p', {}, '驱动就绪不代表热点已开启。请先设置 Wi-Fi 名称与密码。'),

            E('p', {}, [
                E('a', {
                    'href': L.url('admin/network/wireless')
                }, '前往「网络 → 无线」')
            ])
        ]);
    },

    handleStart: function(state, ev) {
        var btn = ev.currentTarget;
        btn.disabled = true;
        state.textContent = '正在提交初始化请求……';

        return fs.exec(command, ['start']).then(function(res) {
            if (res.code !== 0)
                throw new Error(res.stderr || '启动失败');

            state.textContent = '初始化任务已启动，请稍后点击「刷新状态」';
        }).catch(function(err) {
            state.textContent = '启动失败：' + err.message;
        }).finally(function() {
            btn.disabled = false;
        });
    },

    handleRefresh: function(state) {
        return updateStatus(state);
    },

    handleSave: null,
    handleSaveApply: null,
    handleReset: null
});
