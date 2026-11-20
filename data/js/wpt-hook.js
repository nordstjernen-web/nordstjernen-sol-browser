(function (global) {
    'use strict';
    if (global.__ns_wpt_installed) return;
    global.__ns_wpt_installed = true;
    global.__ns_wpt_done = false;
    global.__ns_wpt_seen_harness = false;

    function squash(s) {
        return String(s).replace(/[\u0000-\u001f\u007f-\u009f]+/g, ' ')
                        .replace(/\s+/g, ' ').trim();
    }

    global.__ns_wpt_oncomplete = function (tests, status) {
        if (global.__ns_wpt_done) return;
        var harnessNames = ['OK', 'ERROR', 'TIMEOUT', 'PRECONDITION_FAILED'];
        var subtestNames = ['PASS', 'FAIL', 'TIMEOUT', 'NOTRUN',
                            'PRECONDITION_FAILED'];
        var harness = harnessNames[status.status] ||
                      ('UNKNOWN(' + status.status + ')');
        var head = 'WPT HARNESS ' + harness;
        if (status.message) head += ' | ' + squash(status.message);
        var lines = [head];
        var counts = { PASS: 0, FAIL: 0, TIMEOUT: 0, NOTRUN: 0,
                       PRECONDITION_FAILED: 0 };
        var subtests = [];
        for (var i = 0; i < tests.length; i++) {
            var t = tests[i];
            var st = subtestNames[t.status] || ('UNKNOWN(' + t.status + ')');
            if (counts[st] !== undefined) counts[st]++;
            var line = 'WPT ' + st + ' ' + squash(t.name);
            if (t.message) line += ' | ' + squash(t.message);
            lines.push(line);
            subtests.push({
                name: t.name,
                status: st,
                message: t.message === undefined ? null : t.message
            });
        }
        lines.push('WPT SUMMARY total=' + tests.length +
                   ' pass=' + counts.PASS +
                   ' fail=' + counts.FAIL +
                   ' timeout=' + counts.TIMEOUT +
                   ' notrun=' + counts.NOTRUN +
                   ' precondition_failed=' + counts.PRECONDITION_FAILED);
        global.__ns_wpt_report = lines.join('\n') + '\n';
        global.__ns_wpt_json = JSON.stringify({
            harness: harness,
            message: status.message === undefined ? null : status.message,
            subtests: subtests
        });
        var harnessFailed = harness !== 'OK' &&
                            harness !== 'PRECONDITION_FAILED';
        global.__ns_wpt_failures = counts.FAIL + counts.TIMEOUT +
                                   counts.NOTRUN + (harnessFailed ? 1 : 0);
        global.__ns_wpt_done = true;
    };

    function takeOverTimeout() {
        if (typeof global.timeout !== 'function' ||
            typeof global.setup !== 'function') return;
        var harnessTimeout = global.timeout;
        try {
            global.setup({ explicit_timeout: true });
            global.__ns_wpt_timeout = function () { harnessTimeout(); };
        } catch (e) {}
    }

    function arm(fn) {
        if (global.__ns_wpt_seen_harness) return;
        try {
            fn(global.__ns_wpt_oncomplete);
            global.__ns_wpt_seen_harness = true;
            takeOverTimeout();
        } catch (e) {}
    }

    Object.defineProperty(global, 'add_completion_callback', {
        configurable: true,
        get: function () { return undefined; },
        set: function (fn) {
            delete global.add_completion_callback;
            global.add_completion_callback = fn;
            if (typeof fn !== 'function') return;
            arm(fn);
            if (!global.__ns_wpt_seen_harness) {
                Promise.resolve().then(function () { arm(fn); });
            }
        }
    });

    function elementCenter(element) {
        var x = 0, y = 0;
        try {
            var r = element.getBoundingClientRect();
            x = r.left + r.width / 2;
            y = r.top + r.height / 2;
        } catch (e) {}
        return { x: x, y: y };
    }

    var WEBDRIVER_KEYS = {
        '\uE003': 'Backspace', '\uE004': 'Tab', '\uE006': 'Enter',
        '\uE007': 'Enter', '\uE008': 'Shift', '\uE009': 'Control',
        '\uE00A': 'Alt', '\uE00C': 'Escape', '\uE00D': ' ',
        '\uE00E': 'PageUp', '\uE00F': 'PageDown', '\uE010': 'End',
        '\uE011': 'Home', '\uE012': 'ArrowLeft', '\uE013': 'ArrowUp',
        '\uE014': 'ArrowRight', '\uE015': 'ArrowDown', '\uE017': 'Delete',
        '\uE03D': 'Meta'
    };

    function keyName(ch) {
        return WEBDRIVER_KEYS[ch] || ch;
    }

    function trustedPointer(target, type, x, y, button, buttons) {
        if (typeof global.__nsWptPointer !== 'function') return false;
        try {
            return global.__nsWptPointer(target, type, x || 0, y || 0,
                                         button || 0, buttons || 0) !== undefined;
        } catch (e) { return true; }
    }

    function trustedKey(target, type, key, shift) {
        if (typeof global.__nsWptKey !== 'function') return null;
        try {
            var r = global.__nsWptKey(target, type, key, !!shift);
            return r === undefined ? null : !!r;
        } catch (e) { return false; }
    }

    var NON_TEXT_INPUT_TYPES = ['button', 'submit', 'reset', 'image',
                                'checkbox', 'radio', 'file', 'color',
                                'range', 'hidden'];

    function isTextControl(el) {
        if (!el || !el.tagName) return false;
        var tag = String(el.tagName).toUpperCase();
        if (tag === 'TEXTAREA') return true;
        if (tag !== 'INPUT') return false;
        var t = String(el.type || 'text').toLowerCase();
        return NON_TEXT_INPUT_TYPES.indexOf(t) < 0;
    }

    function fireMouse(target, type, x, y, button) {
        if (!target) return;
        if (trustedPointer(target, type, x, y, button,
                           type === 'mousedown' ? 1 : 0))
            return;
        var ev;
        try {
            ev = new global.MouseEvent(type, {
                bubbles: true, cancelable: true, composed: true,
                view: global, button: button || 0,
                clientX: x || 0, clientY: y || 0
            });
        } catch (e) {
            ev = new global.Event(type, { bubbles: true, cancelable: true });
        }
        try { target.dispatchEvent(ev); } catch (e) {}
    }

    function firePointer(target, type, x, y, pointerType) {
        if (!target) return;
        if (pointerType !== 'touch' &&
            trustedPointer(target, type, x, y, 0,
                           type === 'pointerdown' ? 1 : 0))
            return;
        var ev;
        try {
            ev = new global.PointerEvent(type, {
                bubbles: true, cancelable: true, composed: true,
                view: global, clientX: x || 0, clientY: y || 0,
                pointerType: pointerType || 'mouse', isPrimary: true
            });
        } catch (e) {
            try {
                ev = new global.MouseEvent(type, {
                    bubbles: true, cancelable: true, composed: true,
                    clientX: x || 0, clientY: y || 0
                });
            } catch (e2) { return; }
        }
        try { target.dispatchEvent(ev); } catch (e) {}
    }

    function fireTouch(target, type, x, y) {
        if (!target || typeof global.__nsWptTouch !== 'function') return;
        try { global.__nsWptTouch(target, type, x || 0, y || 0); } catch (e) {}
    }

    function realClick(element) {
        if (!element) return;
        var c = elementCenter(element);
        var pt = 'mouse';
        firePointer(element, 'pointerover', c.x, c.y, pt);
        firePointer(element, 'pointerenter', c.x, c.y, pt);
        firePointer(element, 'pointerdown', c.x, c.y, pt);
        fireMouse(element, 'mousedown', c.x, c.y, 0);
        firePointer(element, 'pointerup', c.x, c.y, pt);
        fireMouse(element, 'mouseup', c.x, c.y, 0);
        try { element.click(); } catch (e) {}
    }

    function sendKeysTo(element, keys) {
        if (!element) return;
        try { element.focus(); } catch (e) {}
        var doc = element.ownerDocument || global.document;
        for (var i = 0; i < keys.length; i++) {
            var ch = keys[i];
            var key = keyName(ch);
            var active = (doc && doc.activeElement) || element;
            var prevented = trustedKey(active, 'keydown', key, false);
            if (prevented === null) {
                var down;
                try {
                    down = new global.KeyboardEvent('keydown',
                        { bubbles: true, cancelable: true, key: key });
                } catch (e) {
                    down = new global.Event('keydown', { bubbles: true, cancelable: true });
                }
                try { prevented = !active.dispatchEvent(down); } catch (e) {}
            }
            if (!prevented && key.length === 1 && isTextControl(active)) {
                try {
                    active.value += key;
                    active.dispatchEvent(new global.Event('input',
                        { bubbles: true }));
                } catch (e) {}
            }
            active = (doc && doc.activeElement) || element;
            if (trustedKey(active, 'keyup', key, false) === null) {
                var up;
                try {
                    up = new global.KeyboardEvent('keyup',
                        { bubbles: true, cancelable: true, key: key });
                } catch (e) {
                    up = new global.Event('keyup', { bubbles: true, cancelable: true });
                }
                try { active.dispatchEvent(up); } catch (e) {}
            }
        }
    }

    function resolveOrigin(origin, state) {
        if (origin && typeof origin === 'object' && origin.nodeType) {
            var c = elementCenter(origin);
            return { target: origin, x: c.x, y: c.y };
        }
        var x = state.x || 0, y = state.y || 0;
        var target = state.target;
        if (!target) {
            try { target = global.document.elementFromPoint(x, y); } catch (e) {}
        }
        return { target: target, x: x, y: y };
    }

    var pointerStates = {};

    function hitAt(state) {
        var hit = null;
        try { hit = global.document.elementFromPoint(state.x, state.y); }
        catch (e) {}
        return hit || state.target;
    }

    function runPointerSource(src) {
        var pt = (src.parameters && src.parameters.pointerType) || 'mouse';
        var id = (src.id || 'pointer') + ':' + pt;
        var state = pointerStates[id] ||
            (pointerStates[id] = { x: 0, y: 0, target: null, downTarget: null });
        var acts = src.actions || [];
        for (var i = 0; i < acts.length; i++) {
            var a = acts[i];
            if (a.type === 'pointerMove') {
                var byElement = a.origin && typeof a.origin === 'object' &&
                    a.origin.nodeType;
                var r = resolveOrigin(a.origin, state);
                var ox = byElement ? r.x : a.origin === 'pointer' ? state.x : 0;
                var oy = byElement ? r.y : a.origin === 'pointer' ? state.y : 0;
                state.x = (a.x || 0) + ox;
                state.y = (a.y || 0) + oy;
                state.target = r.target;
                state.target = hitAt(state);
                firePointer(state.target, 'pointermove', state.x, state.y, pt);
                if (pt === 'mouse')
                    fireMouse(state.target, 'mousemove', state.x, state.y, 0);
                else if (pt === 'touch' && state.downTarget)
                    fireTouch(state.downTarget, 'touchmove', state.x, state.y);
            } else if (a.type === 'pointerDown') {
                state.target = hitAt(state);
                state.downTarget = state.target;
                firePointer(state.target, 'pointerdown', state.x, state.y, pt);
                if (pt === 'mouse')
                    fireMouse(state.target, 'mousedown', state.x, state.y,
                              a.button || 0);
                else if (pt === 'touch')
                    fireTouch(state.target, 'touchstart', state.x, state.y);
            } else if (a.type === 'pointerUp') {
                state.target = hitAt(state);
                firePointer(state.target, 'pointerup', state.x, state.y, pt);
                if (pt === 'mouse')
                    fireMouse(state.target, 'mouseup', state.x, state.y,
                              a.button || 0);
                else if (pt === 'touch')
                    fireTouch(state.downTarget || state.target, 'touchend',
                              state.x, state.y);
                if (state.target && state.target === state.downTarget) {
                    try { state.target.click(); } catch (e) {}
                }
                state.downTarget = null;
            }
        }
    }

    function runKeySource(src) {
        var acts = src.actions || [];
        var doc = global.document;
        var shift = false;
        for (var i = 0; i < acts.length; i++) {
            var a = acts[i];
            if (a.type !== 'keyDown' && a.type !== 'keyUp') continue;
            var type = a.type === 'keyDown' ? 'keydown' : 'keyup';
            var key = keyName(a.value);
            if (key === 'Shift') shift = a.type === 'keyDown';
            var target = (doc && doc.activeElement) || (doc && doc.body);
            if (trustedKey(target || doc, type, key, shift) !== null) continue;
            var ev;
            try {
                ev = new global.KeyboardEvent(type,
                    { bubbles: true, cancelable: true, key: key,
                      shiftKey: shift });
            } catch (e) {
                ev = new global.Event(type, { bubbles: true, cancelable: true });
            }
            try { (target || doc).dispatchEvent(ev); } catch (e) {}
        }
    }

    function runWheelSource(src) {
        if (typeof global.__nsWptWheel !== 'function') return;
        var acts = src.actions || [];
        var state = { x: 0, y: 0, target: null };
        for (var i = 0; i < acts.length; i++) {
            var a = acts[i];
            if (a.type !== 'scroll') continue;
            var usesEl = a.origin && typeof a.origin === 'object' && a.origin.nodeType;
            var r = resolveOrigin(a.origin, state);
            var x = (a.x || 0) + (usesEl ? r.x : 0);
            var y = (a.y || 0) + (usesEl ? r.y : 0);
            var target = null;
            try { target = global.document.elementFromPoint(x, y); } catch (e) {}
            if (!target)
                target = (global.document &&
                    (global.document.scrollingElement || global.document.body));
            try { global.__nsWptWheel(target, x, y, a.deltaX || 0, a.deltaY || 0); }
            catch (e) {}
        }
    }

    function runActions(actions) {
        if (!actions) return;
        for (var i = 0; i < actions.length; i++) {
            var src = actions[i];
            if (!src) continue;
            if (src.type === 'pointer') runPointerSource(src);
            else if (src.type === 'key') runKeySource(src);
            else if (src.type === 'wheel') runWheelSource(src);
        }
    }

    function settle(resolve) {
        if (typeof global.setTimeout === 'function') global.setTimeout(resolve, 0);
        else Promise.resolve().then(resolve);
    }

    function noteActivation() {
        try {
            if (typeof global.__nsWptActivate === 'function')
                global.__nsWptActivate();
        } catch (e) {}
    }

    var bridge = {
        click: function (element) {
            return new Promise(function (resolve) {
                noteActivation();
                try { realClick(element); } catch (e) {}
                settle(resolve);
            });
        },
        send_keys: function (element, keys) {
            return new Promise(function (resolve) {
                noteActivation();
                try { sendKeysTo(element, keys); } catch (e) {}
                settle(resolve);
            });
        },
        action_sequence: function (actions) {
            return new Promise(function (resolve) {
                noteActivation();
                try { runActions(actions); } catch (e) {}
                settle(resolve);
            });
        }
    };

    function patchInternal(obj) {
        if (!obj || typeof obj !== 'object') return obj;
        for (var k in bridge) {
            try { obj[k] = bridge[k]; } catch (e) {}
        }
        return obj;
    }

    var internalStore;
    Object.defineProperty(global, 'test_driver_internal', {
        configurable: true,
        get: function () { return internalStore; },
        set: function (v) { internalStore = patchInternal(v); }
    });
})(typeof globalThis !== 'undefined' ? globalThis : this);
