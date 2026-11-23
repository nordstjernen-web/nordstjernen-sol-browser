/* typed-om.js: CSS Typed OM (CSSStyleValue, CSSNumericValue, StylePropertyMap) over the engine's CSSOM. */
(function (global) {
    'use strict';

    if (typeof global.CSSStyleValue === 'function') return;

    var nativeComputedValue = typeof global.__ndComputedValue === 'function' ? global.__ndComputedValue : null;
    try { delete global.__ndComputedValue; } catch (e) {}

    var slots = new WeakMap();
    var internalConstruct = false;

    function slot(o) {
        var s = slots.get(o);
        if (!s) throw new TypeError('Illegal invocation');
        return s;
    }

    function syntaxError(msg) {
        return new DOMException(msg || 'Invalid CSS', 'SyntaxError');
    }

    function define(name, ctor) {
        Object.defineProperty(ctor.prototype, Symbol.toStringTag, {
            value: name, configurable: true
        });
        Object.defineProperty(global, name, {
            value: ctor, writable: true, configurable: true, enumerable: false
        });
    }

    function accessor(proto, name, get, set) {
        Object.defineProperty(proto, name, {
            get: get, set: set, enumerable: true, configurable: true
        });
    }

    var UNIT_BASE = {
        number: null, percent: 'percent',
        em: 'length', ex: 'length', ch: 'length', ic: 'length', cap: 'length',
        rem: 'length', rex: 'length', rch: 'length', ric: 'length', rcap: 'length',
        lh: 'length', rlh: 'length',
        vw: 'length', vh: 'length', vi: 'length', vb: 'length', vmin: 'length', vmax: 'length',
        svw: 'length', svh: 'length', svi: 'length', svb: 'length', svmin: 'length', svmax: 'length',
        lvw: 'length', lvh: 'length', lvi: 'length', lvb: 'length', lvmin: 'length', lvmax: 'length',
        dvw: 'length', dvh: 'length', dvi: 'length', dvb: 'length', dvmin: 'length', dvmax: 'length',
        cqw: 'length', cqh: 'length', cqi: 'length', cqb: 'length', cqmin: 'length', cqmax: 'length',
        cm: 'length', mm: 'length', q: 'length', in: 'length', pt: 'length', pc: 'length', px: 'length',
        deg: 'angle', grad: 'angle', rad: 'angle', turn: 'angle',
        s: 'time', ms: 'time',
        hz: 'frequency', khz: 'frequency',
        dpi: 'resolution', dpcm: 'resolution', dppx: 'resolution', x: 'resolution',
        fr: 'flex'
    };

    var CANONICAL = {
        px: ['px', 1], cm: ['px', 96 / 2.54], mm: ['px', 96 / 25.4], q: ['px', 96 / 101.6],
        in: ['px', 96], pt: ['px', 4 / 3], pc: ['px', 16],
        deg: ['deg', 1], rad: ['deg', 180 / Math.PI], grad: ['deg', 0.9], turn: ['deg', 360],
        s: ['s', 1], ms: ['s', 0.001],
        hz: ['hz', 1], khz: ['hz', 1000],
        dppx: ['dppx', 1], x: ['dppx', 1], dpi: ['dppx', 1 / 96], dpcm: ['dppx', 2.54 / 96]
    };

    var BASE_TYPES = ['length', 'angle', 'time', 'frequency', 'resolution', 'flex', 'percent'];

    function normalizeUnit(unit) {
        var u = String(unit);
        if (u === '%') return 'percent';
        u = u.toLowerCase();
        return Object.prototype.hasOwnProperty.call(UNIT_BASE, u) ? u : null;
    }

    function newType() {
        return { length: 0, angle: 0, time: 0, frequency: 0, resolution: 0, flex: 0, percent: 0, hint: null };
    }

    function copyType(t) {
        var c = newType();
        for (var i = 0; i < BASE_TYPES.length; i++) c[BASE_TYPES[i]] = t[BASE_TYPES[i]];
        c.hint = t.hint;
        return c;
    }

    function typeOfUnit(unit) {
        var t = newType();
        var base = UNIT_BASE[unit];
        if (base) t[base] = 1;
        return t;
    }

    function applyHint(t, hint) {
        if (t.percent) {
            t[hint] += t.percent;
            t.percent = 0;
        }
        t.hint = hint;
    }

    function sameEntries(a, b) {
        for (var i = 0; i < BASE_TYPES.length; i++)
            if (a[BASE_TYPES[i]] !== b[BASE_TYPES[i]]) return false;
        return true;
    }

    function hasNonPercent(t) {
        for (var i = 0; i < BASE_TYPES.length; i++)
            if (BASE_TYPES[i] !== 'percent' && t[BASE_TYPES[i]] !== 0) return true;
        return false;
    }

    function addTypes(a, b) {
        if (!a || !b) return null;
        a = copyType(a);
        b = copyType(b);
        if (a.hint && b.hint && a.hint !== b.hint) return null;
        if (a.hint && !b.hint) applyHint(b, a.hint);
        else if (b.hint && !a.hint) applyHint(a, b.hint);
        if (sameEntries(a, b)) return a;
        if ((a.percent || b.percent) && (hasNonPercent(a) || hasNonPercent(b))) {
            for (var i = 0; i < BASE_TYPES.length; i++) {
                var hint = BASE_TYPES[i];
                if (hint === 'percent') continue;
                var a2 = copyType(a), b2 = copyType(b);
                applyHint(a2, hint);
                applyHint(b2, hint);
                if (sameEntries(a2, b2)) return a2;
            }
        }
        return null;
    }

    function multiplyTypes(a, b) {
        if (!a || !b) return null;
        a = copyType(a);
        b = copyType(b);
        if (a.hint && b.hint && a.hint !== b.hint) return null;
        if (a.hint && !b.hint) applyHint(b, a.hint);
        else if (b.hint && !a.hint) applyHint(a, b.hint);
        var r = newType();
        for (var i = 0; i < BASE_TYPES.length; i++)
            r[BASE_TYPES[i]] = a[BASE_TYPES[i]] + b[BASE_TYPES[i]];
        r.hint = a.hint || b.hint;
        return r;
    }

    function invertType(t) {
        var r = copyType(t);
        for (var i = 0; i < BASE_TYPES.length; i++) r[BASE_TYPES[i]] = -r[BASE_TYPES[i]] || 0;
        return r;
    }

    function onlyBase(t, base) {
        if (!t) return false;
        for (var i = 0; i < BASE_TYPES.length; i++)
            if (t[BASE_TYPES[i]] !== (BASE_TYPES[i] === base ? 1 : 0)) return false;
        return true;
    }

    function isNumberType(t) { return onlyBase(t, null) && !t.hint; }
    function isLengthType(t, percentOk) {
        if (onlyBase(t, 'length')) return !t.hint || (percentOk && t.hint === 'length');
        return !!percentOk && onlyBase(t, 'percent') && !t.hint;
    }
    function isAngleType(t) { return onlyBase(t, 'angle') && !t.hint; }

    function formatNumber(v) {
        if (v === Infinity) return 'infinity';
        if (v === -Infinity) return '-infinity';
        if (v !== v) return 'NaN';
        if (Object.is(v, -0)) return '0';
        if (Number.isInteger(v) && Math.abs(v) < 1e21) return String(v);
        return String(+v.toPrecision(6));
    }

    function rectify(x) {
        if (x instanceof CSSNumericValue) return x;
        var n = typeof x === 'number' ? x : Number(x);
        if (typeof x !== 'number' && (typeof x === 'object' || typeof x === 'function') && x !== null)
            throw new TypeError('Expected a number or CSSNumericValue');
        if (!isFinite(n)) throw new TypeError('The value is not a finite number');
        return makeUnit(n, 'number');
    }

    function finiteDouble(x) {
        var n = Number(x);
        if (!isFinite(n)) throw new TypeError('The value is not a finite number');
        return n;
    }

    function CSSStyleValue() {
        if (!internalConstruct && (new.target === CSSStyleValue || new.target === undefined))
            throw new TypeError('Illegal constructor');
        if (!slots.has(this)) slots.set(this, {});
    }
    CSSStyleValue.prototype.toString = function () {
        var s = slot(this);
        return s.text !== undefined ? s.text : '';
    };

    function makeOpaque(text, property, ctor) {
        internalConstruct = true;
        var v;
        try { v = new (ctor || CSSStyleValue)(); } finally { internalConstruct = false; }
        var s = slot(v);
        s.text = text;
        s.property = property;
        return v;
    }

    function CSSNumericValue() {
        if (!internalConstruct && (new.target === CSSNumericValue || new.target === undefined))
            throw new TypeError('Illegal constructor');
        CSSStyleValue.call(this);
    }
    CSSNumericValue.prototype = Object.create(CSSStyleValue.prototype, {
        constructor: { value: CSSNumericValue, writable: true, configurable: true }
    });
    Object.setPrototypeOf(CSSNumericValue, CSSStyleValue);

    function numericType(v) {
        return slot(v).type;
    }

    function makeUnit(value, unit) {
        return new CSSUnitValue(value, unit);
    }

    function negateValue(v) {
        if (v instanceof CSSMathNegate) return v.value;
        if (v instanceof CSSUnitValue) return makeUnit(-v.value, v.unit);
        return new CSSMathNegate(v);
    }

    function invertValue(v) {
        if (v instanceof CSSMathInvert) return v.value;
        if (v instanceof CSSUnitValue && v.unit === 'number') {
            if (v.value === 0) throw new RangeError('Cannot divide by zero');
            return makeUnit(1 / v.value, 'number');
        }
        return new CSSMathInvert(v);
    }

    function allSameUnit(values) {
        var unit = null;
        for (var i = 0; i < values.length; i++) {
            if (!(values[i] instanceof CSSUnitValue)) return null;
            if (unit === null) unit = values[i].unit;
            else if (values[i].unit !== unit) return null;
        }
        return unit;
    }

    CSSNumericValue.prototype.add = function () {
        var values = Array.prototype.map.call(arguments, rectify);
        values = (this instanceof CSSMathSum ? toArray(this.values) : [this]).concat(values);
        var unit = allSameUnit(values);
        if (unit !== null) {
            var sum = 0;
            for (var i = 0; i < values.length; i++) sum += values[i].value;
            return makeUnit(sum, unit);
        }
        return construct(CSSMathSum, values);
    };

    CSSNumericValue.prototype.sub = function () {
        var values = Array.prototype.map.call(arguments, function (v) { return negateValue(rectify(v)); });
        return this.add.apply(this, values);
    };

    CSSNumericValue.prototype.mul = function () {
        var values = Array.prototype.map.call(arguments, rectify);
        values = (this instanceof CSSMathProduct ? toArray(this.values) : [this]).concat(values);
        var allUnits = values.every(function (v) { return v instanceof CSSUnitValue; });
        if (allUnits) {
            var nonNumber = values.filter(function (v) { return v.unit !== 'number'; });
            if (nonNumber.length <= 1) {
                var product = 1;
                for (var i = 0; i < values.length; i++) product *= values[i].value;
                return makeUnit(product, nonNumber.length ? nonNumber[0].unit : 'number');
            }
        }
        return construct(CSSMathProduct, values);
    };

    CSSNumericValue.prototype.div = function () {
        var values = Array.prototype.map.call(arguments, function (v) { return invertValue(rectify(v)); });
        return this.mul.apply(this, values);
    };

    function minMax(self, args, ctor, pick) {
        var values = Array.prototype.map.call(args, rectify);
        values = (self instanceof ctor ? toArray(self.values) : [self]).concat(values);
        var unit = allSameUnit(values);
        if (unit !== null) {
            var best = values[0].value;
            for (var i = 1; i < values.length; i++) best = pick(best, values[i].value);
            return makeUnit(best, unit);
        }
        return construct(ctor, values);
    }

    CSSNumericValue.prototype.min = function () { return minMax(this, arguments, CSSMathMin, Math.min); };
    CSSNumericValue.prototype.max = function () { return minMax(this, arguments, CSSMathMax, Math.max); };

    function numericEquals(a, b) {
        if (a === b) return true;
        if (!a || !b || Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;
        if (a instanceof CSSUnitValue) return a.value === b.value && a.unit === b.unit;
        if (a instanceof CSSMathNegate || a instanceof CSSMathInvert) return numericEquals(a.value, b.value);
        if (a instanceof CSSMathClamp)
            return numericEquals(a.lower, b.lower) && numericEquals(a.value, b.value) &&
                numericEquals(a.upper, b.upper);
        var av = toArray(a.values), bv = toArray(b.values);
        if (av.length !== bv.length) return false;
        for (var i = 0; i < av.length; i++) if (!numericEquals(av[i], bv[i])) return false;
        return true;
    }

    CSSNumericValue.prototype.equals = function () {
        var self = this;
        var values = Array.prototype.map.call(arguments, rectify);
        return values.every(function (v) { return numericEquals(self, v); });
    };

    function unitMapKey(units) {
        return Object.keys(units).sort().map(function (k) { return k + '^' + units[k]; }).join(' ');
    }

    function sumValue(v) {
        if (v instanceof CSSUnitValue) {
            var unit = v.unit, value = v.value;
            var c = CANONICAL[unit];
            if (c) { value *= c[1]; unit = c[0]; }
            var units = {};
            if (unit !== 'number') units[unit] = 1;
            return [{ value: value, units: units }];
        }
        var items, i, j;
        if (v instanceof CSSMathSum) {
            var all = [];
            var vals = toArray(v.values);
            for (i = 0; i < vals.length; i++) {
                var s = sumValue(vals[i]);
                if (!s) return null;
                for (j = 0; j < s.length; j++) {
                    var key = unitMapKey(s[j].units), merged = false;
                    for (var k = 0; k < all.length; k++) {
                        if (unitMapKey(all[k].units) === key) {
                            all[k].value += s[j].value;
                            merged = true;
                            break;
                        }
                    }
                    if (!merged) all.push({ value: s[j].value, units: Object.assign({}, s[j].units) });
                }
            }
            return all;
        }
        if (v instanceof CSSMathNegate) {
            items = sumValue(v.value);
            if (!items) return null;
            return items.map(function (it) { return { value: -it.value, units: it.units }; });
        }
        if (v instanceof CSSMathProduct) {
            var result = [{ value: 1, units: {} }];
            var pv = toArray(v.values);
            for (i = 0; i < pv.length; i++) {
                var child = sumValue(pv[i]);
                if (!child) return null;
                var next = [];
                for (j = 0; j < result.length; j++) {
                    for (var m = 0; m < child.length; m++) {
                        var u = Object.assign({}, result[j].units);
                        for (var name in child[m].units) {
                            u[name] = (u[name] || 0) + child[m].units[name];
                            if (u[name] === 0) delete u[name];
                        }
                        next.push({ value: result[j].value * child[m].value, units: u });
                    }
                }
                result = next;
            }
            return result;
        }
        if (v instanceof CSSMathInvert) {
            items = sumValue(v.value);
            if (!items || items.length !== 1) return null;
            var inv = {};
            for (var n in items[0].units) inv[n] = -items[0].units[n];
            return [{ value: 1 / items[0].value, units: inv }];
        }
        if (v instanceof CSSMathMin || v instanceof CSSMathMax || v instanceof CSSMathClamp) {
            var args = v instanceof CSSMathClamp ? [v.lower, v.value, v.upper] : toArray(v.values);
            var sums = [];
            for (i = 0; i < args.length; i++) {
                var a = sumValue(args[i]);
                if (!a || a.length !== 1) return null;
                if (sums.length && unitMapKey(sums[0].units) !== unitMapKey(a[0].units)) return null;
                sums.push(a[0]);
            }
            if (v instanceof CSSMathClamp) {
                var clamped = Math.max(sums[0].value, Math.min(sums[1].value, sums[2].value));
                return [{ value: clamped, units: sums[0].units }];
            }
            var pick = sums[0];
            for (i = 1; i < sums.length; i++) {
                if (v instanceof CSSMathMin ? sums[i].value < pick.value : sums[i].value > pick.value)
                    pick = sums[i];
            }
            return [pick];
        }
        return null;
    }

    function sumItemToUnit(item) {
        var names = Object.keys(item.units);
        if (names.length === 0) return makeUnit(item.value, 'number');
        if (names.length === 1 && item.units[names[0]] === 1) return makeUnit(item.value, names[0]);
        return null;
    }

    function convertUnit(value, unit) {
        var from = value.unit, to = unit;
        if (from === to) return makeUnit(value.value, to);
        var cf = CANONICAL[from], ct = CANONICAL[to];
        if (!cf || !ct || cf[0] !== ct[0]) return null;
        return makeUnit(value.value * cf[1] / ct[1], to);
    }

    CSSNumericValue.prototype.to = function (unit) {
        var u = normalizeUnit(unit);
        if (u === null) throw syntaxError('Invalid unit');
        var sum = sumValue(this);
        if (!sum || sum.length !== 1) throw new TypeError('Cannot convert to a single unit');
        var item = sumItemToUnit(sum[0]);
        var converted = item && convertUnit(item, u);
        if (!converted) throw new TypeError('Incompatible units');
        return converted;
    };

    CSSNumericValue.prototype.toSum = function () {
        var units = Array.prototype.map.call(arguments, function (u) {
            var n = normalizeUnit(u);
            if (n === null) throw syntaxError('Invalid unit');
            return n;
        });
        var sum = sumValue(this);
        if (!sum) throw new TypeError('Cannot create a sum value');
        var values = [];
        for (var i = 0; i < sum.length; i++) {
            var uv = sumItemToUnit(sum[i]);
            if (!uv) throw new TypeError('Cannot create a sum value');
            values.push(uv);
        }
        if (!units.length) {
            values.sort(function (a, b) { return a.unit < b.unit ? -1 : a.unit > b.unit ? 1 : 0; });
            return construct(CSSMathSum, values);
        }
        var result = [];
        for (var k = 0; k < units.length; k++) {
            var total = 0;
            values = values.filter(function (v) {
                var c = convertUnit(v, units[k]);
                if (!c) return true;
                total += c.value;
                return false;
            });
            result.push(makeUnit(total, units[k]));
        }
        if (values.length) throw new TypeError('Not all values could be converted');
        return construct(CSSMathSum, result);
    };

    CSSNumericValue.prototype.type = function () {
        var t = numericType(this);
        var out = {};
        for (var i = 0; i < BASE_TYPES.length; i++)
            if (t[BASE_TYPES[i]] !== 0) out[BASE_TYPES[i]] = t[BASE_TYPES[i]];
        if (t.hint) out.percentHint = t.hint;
        return out;
    };

    CSSNumericValue.parse = function (cssText) {
        var text = String(cssText).trim();
        var v;
        try { v = parseNumeric(text); } catch (e) { v = null; }
        if (!v) throw syntaxError('Not a numeric value');
        return v;
    };

    function CSSUnitValue(value, unit) {
        if (new.target === undefined) throw new TypeError("Constructor requires 'new'");
        var n = finiteDouble(value);
        var u = normalizeUnit(unit);
        if (u === null) throw new TypeError('Invalid unit: ' + unit);
        internalConstruct = true;
        try { CSSNumericValue.call(this); } finally { internalConstruct = false; }
        var s = slot(this);
        s.value = n;
        s.unit = u;
        s.type = typeOfUnit(u);
    }
    CSSUnitValue.prototype = Object.create(CSSNumericValue.prototype, {
        constructor: { value: CSSUnitValue, writable: true, configurable: true }
    });
    Object.setPrototypeOf(CSSUnitValue, CSSNumericValue);
    accessor(CSSUnitValue.prototype, 'value',
        function () { return slot(this).value; },
        function (v) { slot(this).value = finiteDouble(v); });
    accessor(CSSUnitValue.prototype, 'unit', function () { return slot(this).unit; });
    CSSUnitValue.prototype.toString = function () {
        var s = slot(this);
        var suffix = s.unit === 'number' ? '' : s.unit === 'percent' ? '%' : s.unit;
        return formatNumber(s.value) + suffix;
    };

    function CSSNumericArray() {
        if (!internalConstruct) throw new TypeError('Illegal constructor');
    }
    CSSNumericArray.prototype[Symbol.iterator] = Array.prototype.values;
    CSSNumericArray.prototype.entries = Array.prototype.entries;
    CSSNumericArray.prototype.keys = Array.prototype.keys;
    CSSNumericArray.prototype.values = Array.prototype.values;
    CSSNumericArray.prototype.forEach = Array.prototype.forEach;
    accessor(CSSNumericArray.prototype, 'length', function () { return slot(this).items.length; });

    function makeNumericArray(items) {
        internalConstruct = true;
        var a;
        try { a = new CSSNumericArray(); } finally { internalConstruct = false; }
        slots.set(a, { items: items.slice() });
        for (var i = 0; i < items.length; i++)
            Object.defineProperty(a, i, { value: items[i], enumerable: true, writable: false, configurable: false });
        return a;
    }

    function toArray(list) {
        var s = slots.get(list);
        return s && s.items ? s.items.slice() : Array.prototype.slice.call(list);
    }

    function construct(ctor, values) {
        return new (Function.prototype.bind.apply(ctor, [null].concat(values)))();
    }

    function CSSMathValue() {
        if (!internalConstruct && (new.target === CSSMathValue || new.target === undefined))
            throw new TypeError('Illegal constructor');
        internalConstruct = true;
        try { CSSNumericValue.call(this); } finally { internalConstruct = false; }
    }
    CSSMathValue.prototype = Object.create(CSSNumericValue.prototype, {
        constructor: { value: CSSMathValue, writable: true, configurable: true }
    });
    Object.setPrototypeOf(CSSMathValue, CSSNumericValue);
    accessor(CSSMathValue.prototype, 'operator', function () { return slot(this).operator; });

    function serializeMath(v, nested, parenLess) {
        if (v instanceof CSSUnitValue) return v.toString();
        var s = slot(v);
        var parts, i;
        if (s.operator === 'min' || s.operator === 'max') {
            parts = s.items.map(function (a) { return serializeMath(a, false, true); });
            return s.operator + '(' + parts.join(', ') + ')';
        }
        if (s.operator === 'clamp') {
            parts = [s.lower, s.value, s.upper].map(function (a) { return serializeMath(a, false, true); });
            return 'clamp(' + parts.join(', ') + ')';
        }
        var out = parenLess ? '' : nested ? '(' : 'calc(';
        if (s.operator === 'sum') {
            out += serializeMath(s.items[0], true, false);
            for (i = 1; i < s.items.length; i++) {
                var item = s.items[i];
                if (item instanceof CSSMathNegate) out += ' - ' + serializeMath(item.value, true, false);
                else out += ' + ' + serializeMath(item, true, false);
            }
        } else if (s.operator === 'negate') {
            out += '-' + serializeMath(s.value, true, false);
        } else if (s.operator === 'product') {
            out += serializeMath(s.items[0], true, false);
            for (i = 1; i < s.items.length; i++) {
                var f = s.items[i];
                if (f instanceof CSSMathInvert) out += ' / ' + serializeMath(f.value, true, false);
                else out += ' * ' + serializeMath(f, true, false);
            }
        } else if (s.operator === 'invert') {
            out += '1 / ' + serializeMath(s.value, true, false);
        }
        if (!parenLess) out += ')';
        return out;
    }

    CSSMathValue.prototype.toString = function () { return serializeMath(this, false, false); };

    function mathVariadic(name, operator, combine) {
        var ctor = function () {
            if (new.target === undefined) throw new TypeError("Constructor requires 'new'");
            var items = Array.prototype.map.call(arguments, rectify);
            if (!items.length) throw syntaxError(name + ' needs at least one argument');
            var type = numericType(items[0]);
            for (var i = 1; i < items.length && type; i++) type = combine(type, numericType(items[i]));
            if (!type) throw new TypeError('Incompatible types in ' + name);
            internalConstruct = true;
            try { CSSMathValue.call(this); } finally { internalConstruct = false; }
            var s = slot(this);
            s.operator = operator;
            s.items = items;
            s.array = makeNumericArray(items);
            s.type = type;
        };
        Object.defineProperty(ctor, 'name', { value: name });
        ctor.prototype = Object.create(CSSMathValue.prototype, {
            constructor: { value: ctor, writable: true, configurable: true }
        });
        Object.setPrototypeOf(ctor, CSSMathValue);
        accessor(ctor.prototype, 'values', function () { return slot(this).array; });
        return ctor;
    }

    var CSSMathSum = mathVariadic('CSSMathSum', 'sum', addTypes);
    var CSSMathProduct = mathVariadic('CSSMathProduct', 'product', multiplyTypes);
    var CSSMathMin = mathVariadic('CSSMathMin', 'min', addTypes);
    var CSSMathMax = mathVariadic('CSSMathMax', 'max', addTypes);

    function mathUnary(name, operator, typeFn) {
        var ctor = function (arg) {
            if (new.target === undefined) throw new TypeError("Constructor requires 'new'");
            var v = rectify(arg);
            internalConstruct = true;
            try { CSSMathValue.call(this); } finally { internalConstruct = false; }
            var s = slot(this);
            s.operator = operator;
            s.value = v;
            s.type = typeFn(numericType(v));
        };
        Object.defineProperty(ctor, 'name', { value: name });
        ctor.prototype = Object.create(CSSMathValue.prototype, {
            constructor: { value: ctor, writable: true, configurable: true }
        });
        Object.setPrototypeOf(ctor, CSSMathValue);
        accessor(ctor.prototype, 'value', function () { return slot(this).value; });
        return ctor;
    }

    var CSSMathNegate = mathUnary('CSSMathNegate', 'negate', copyType);
    var CSSMathInvert = mathUnary('CSSMathInvert', 'invert', invertType);

    function CSSMathClamp(lower, value, upper) {
        if (new.target === undefined) throw new TypeError("Constructor requires 'new'");
        var l = rectify(lower), v = rectify(value), u = rectify(upper);
        var type = addTypes(addTypes(numericType(l), numericType(v)), numericType(u));
        if (!type) throw new TypeError('Incompatible types in CSSMathClamp');
        internalConstruct = true;
        try { CSSMathValue.call(this); } finally { internalConstruct = false; }
        var s = slot(this);
        s.operator = 'clamp';
        s.lower = l;
        s.value = v;
        s.upper = u;
        s.type = type;
    }
    CSSMathClamp.prototype = Object.create(CSSMathValue.prototype, {
        constructor: { value: CSSMathClamp, writable: true, configurable: true }
    });
    Object.setPrototypeOf(CSSMathClamp, CSSMathValue);
    accessor(CSSMathClamp.prototype, 'lower', function () { return slot(this).lower; });
    accessor(CSSMathClamp.prototype, 'value', function () { return slot(this).value; });
    accessor(CSSMathClamp.prototype, 'upper', function () { return slot(this).upper; });

    function checkKeyword(value) {
        var s = String(value);
        if (s === '') throw new TypeError('A keyword cannot be empty');
        return s;
    }

    function CSSKeywordValue(value) {
        if (new.target === undefined) throw new TypeError("Constructor requires 'new'");
        var v = checkKeyword(value);
        internalConstruct = true;
        try { CSSStyleValue.call(this); } finally { internalConstruct = false; }
        slot(this).value = v;
    }
    CSSKeywordValue.prototype = Object.create(CSSStyleValue.prototype, {
        constructor: { value: CSSKeywordValue, writable: true, configurable: true }
    });
    Object.setPrototypeOf(CSSKeywordValue, CSSStyleValue);
    accessor(CSSKeywordValue.prototype, 'value',
        function () { return slot(this).value; },
        function (v) { slot(this).value = checkKeyword(v); });
    CSSKeywordValue.prototype.toString = function () {
        var v = slot(this).value;
        return /^-?[A-Za-z_\u0080-￿][\w\u0080-￿-]*$/.test(v) || /^--/.test(v)
            ? v : (global.CSS && CSS.escape ? CSS.escape(v) : v);
    };

    function checkVariable(name) {
        var s = String(name);
        if (s.slice(0, 2) !== '--') throw new TypeError('A custom property name must start with --');
        return s;
    }

    function CSSVariableReferenceValue(variable, fallback) {
        if (new.target === undefined) throw new TypeError("Constructor requires 'new'");
        var name = checkVariable(variable);
        if (fallback !== undefined && fallback !== null && !(fallback instanceof CSSUnparsedValue))
            throw new TypeError('The fallback must be a CSSUnparsedValue');
        slots.set(this, { variable: name, fallback: fallback === undefined ? null : fallback });
    }
    accessor(CSSVariableReferenceValue.prototype, 'variable',
        function () { return slot(this).variable; },
        function (v) { slot(this).variable = checkVariable(v); });
    accessor(CSSVariableReferenceValue.prototype, 'fallback', function () { return slot(this).fallback; });
    CSSVariableReferenceValue.prototype.toString = function () {
        var s = slot(this);
        return 'var(' + s.variable + (s.fallback ? ',' + s.fallback.toString() : '') + ')';
    };

    function checkMember(m) {
        if (m instanceof CSSVariableReferenceValue) return m;
        if (typeof m === 'object' && m !== null && !(m instanceof String))
            throw new TypeError('Members must be strings or CSSVariableReferenceValues');
        return String(m);
    }

    function indexOf(key) {
        if (typeof key !== 'string' || !/^(0|[1-9]\d*)$/.test(key)) return -1;
        var n = Number(key);
        return n < 4294967295 ? n : -1;
    }

    function listProxy(target, items, check) {
        return new Proxy(target, {
            get: function (o, k, r) {
                var i = indexOf(k);
                if (i >= 0) return items[i];
                return Reflect.get(o, k, r);
            },
            set: function (o, k, v, r) {
                var i = indexOf(k);
                if (i < 0) return Reflect.set(o, k, v, r);
                if (i > items.length) throw new RangeError('Index out of range');
                items[i] = check(v);
                return true;
            },
            has: function (o, k) {
                var i = indexOf(k);
                if (i >= 0) return i < items.length;
                return Reflect.has(o, k);
            },
            ownKeys: function (o) {
                var keys = [];
                for (var i = 0; i < items.length; i++) keys.push(String(i));
                return keys.concat(Reflect.ownKeys(o));
            },
            getOwnPropertyDescriptor: function (o, k) {
                var i = indexOf(k);
                if (i >= 0 && i < items.length)
                    return { value: items[i], writable: true, enumerable: true, configurable: true };
                return Reflect.getOwnPropertyDescriptor(o, k);
            },
            defineProperty: function (o, k, d) {
                if (indexOf(k) >= 0) return false;
                return Reflect.defineProperty(o, k, d);
            },
            deleteProperty: function (o, k) {
                if (indexOf(k) >= 0) return false;
                return Reflect.deleteProperty(o, k);
            }
        });
    }

    function CSSUnparsedValue(members) {
        if (new.target === undefined) throw new TypeError("Constructor requires 'new'");
        if (members === undefined || members === null || typeof members[Symbol.iterator] !== 'function')
            throw new TypeError('CSSUnparsedValue requires a sequence');
        var items = Array.from(members, checkMember);
        internalConstruct = true;
        try { CSSStyleValue.call(this); } finally { internalConstruct = false; }
        slot(this).items = items;
        var proxy = listProxy(this, items, checkMember);
        slots.set(proxy, slot(this));
        return proxy;
    }
    CSSUnparsedValue.prototype = Object.create(CSSStyleValue.prototype, {
        constructor: { value: CSSUnparsedValue, writable: true, configurable: true }
    });
    Object.setPrototypeOf(CSSUnparsedValue, CSSStyleValue);
    accessor(CSSUnparsedValue.prototype, 'length', function () { return slot(this).items.length; });
    CSSUnparsedValue.prototype[Symbol.iterator] = function () { return slot(this).items.slice()[Symbol.iterator](); };
    CSSUnparsedValue.prototype.entries = function () { return slot(this).items.slice().entries(); };
    CSSUnparsedValue.prototype.keys = function () { return slot(this).items.slice().keys(); };
    CSSUnparsedValue.prototype.values = function () { return slot(this).items.slice().values(); };
    CSSUnparsedValue.prototype.forEach = function (cb, thisArg) {
        var items = slot(this).items.slice();
        for (var i = 0; i < items.length; i++) cb.call(thisArg, items[i], i, this);
    };
    CSSUnparsedValue.prototype.toString = function () {
        return slot(this).items.map(function (m) { return String(m); }).join('');
    };

    function CSSImageValue() {
        if (!internalConstruct) throw new TypeError('Illegal constructor');
        CSSStyleValue.call(this);
    }
    CSSImageValue.prototype = Object.create(CSSStyleValue.prototype, {
        constructor: { value: CSSImageValue, writable: true, configurable: true }
    });
    Object.setPrototypeOf(CSSImageValue, CSSStyleValue);

    function CSSTransformComponent() {
        if (!internalConstruct && (new.target === CSSTransformComponent || new.target === undefined))
            throw new TypeError('Illegal constructor');
        if (!slots.has(this)) slots.set(this, {});
    }
    accessor(CSSTransformComponent.prototype, 'is2D',
        function () { return slot(this).is2D; },
        function (v) { if (!slot(this).fixed2D) slot(this).is2D = !!v; });
    CSSTransformComponent.prototype.toMatrix = function () {
        return componentMatrix(this);
    };

    function transformSubclass(name, init, toString) {
        var ctor = function () {
            if (new.target === undefined) throw new TypeError("Constructor requires 'new'");
            internalConstruct = true;
            try { CSSTransformComponent.call(this); } finally { internalConstruct = false; }
            init.apply(this, [slot(this)].concat(Array.prototype.slice.call(arguments)));
        };
        Object.defineProperty(ctor, 'name', { value: name });
        ctor.prototype = Object.create(CSSTransformComponent.prototype, {
            constructor: { value: ctor, writable: true, configurable: true }
        });
        Object.setPrototypeOf(ctor, CSSTransformComponent);
        ctor.prototype.toString = function () { return toString(slot(this)); };
        define(name, ctor);
        return ctor;
    }

    function checkLengthPercent(v) {
        if (!(v instanceof CSSNumericValue) || !isLengthType(numericType(v), true))
            throw new TypeError('Expected a <length-percentage>');
        return v;
    }
    function checkLength(v) {
        if (!(v instanceof CSSNumericValue) || !isLengthType(numericType(v), false))
            throw new TypeError('Expected a <length>');
        return v;
    }
    function checkAngle(v) {
        if (!(v instanceof CSSNumericValue) || !isAngleType(numericType(v)))
            throw new TypeError('Expected an <angle>');
        return v;
    }
    function checkNumberish(v) {
        var r = rectify(v);
        if (!isNumberType(numericType(r))) throw new TypeError('Expected a <number>');
        return r;
    }

    function axisAccessors(ctor, names, check) {
        names.forEach(function (n) {
            accessor(ctor.prototype, n,
                function () { return slot(this)[n]; },
                function (v) { slot(this)[n] = check(v); });
        });
    }

    var CSSTranslate = transformSubclass('CSSTranslate', function (s, x, y, z) {
        s.x = checkLengthPercent(x);
        s.y = checkLengthPercent(y);
        if (z === undefined) { s.z = makeUnit(0, 'px'); s.is2D = true; }
        else { s.z = checkLength(z); s.is2D = false; }
    }, function (s) {
        return s.is2D ? 'translate(' + s.x + ', ' + s.y + ')'
            : 'translate3d(' + s.x + ', ' + s.y + ', ' + s.z + ')';
    });
    axisAccessors(CSSTranslate, ['x', 'y'], checkLengthPercent);
    axisAccessors(CSSTranslate, ['z'], checkLength);

    var CSSRotate = transformSubclass('CSSRotate', function (s, a, b, c, d) {
        if (arguments.length <= 2) {
            s.x = makeUnit(0, 'number');
            s.y = makeUnit(0, 'number');
            s.z = makeUnit(1, 'number');
            s.angle = checkAngle(a);
            s.is2D = true;
        } else {
            s.x = checkNumberish(a);
            s.y = checkNumberish(b);
            s.z = checkNumberish(c);
            s.angle = checkAngle(d);
            s.is2D = false;
        }
    }, function (s) {
        return s.is2D ? 'rotate(' + s.angle + ')'
            : 'rotate3d(' + s.x + ', ' + s.y + ', ' + s.z + ', ' + s.angle + ')';
    });
    axisAccessors(CSSRotate, ['x', 'y', 'z'], checkNumberish);
    axisAccessors(CSSRotate, ['angle'], checkAngle);

    var CSSScale = transformSubclass('CSSScale', function (s, x, y, z) {
        s.x = checkNumberish(x);
        s.y = checkNumberish(y);
        if (z === undefined) { s.z = makeUnit(1, 'number'); s.is2D = true; }
        else { s.z = checkNumberish(z); s.is2D = false; }
    }, function (s) {
        return s.is2D ? 'scale(' + s.x + ', ' + s.y + ')'
            : 'scale3d(' + s.x + ', ' + s.y + ', ' + s.z + ')';
    });
    axisAccessors(CSSScale, ['x', 'y', 'z'], checkNumberish);

    var CSSSkew = transformSubclass('CSSSkew', function (s, ax, ay) {
        s.ax = checkAngle(ax);
        s.ay = checkAngle(ay);
        s.is2D = true;
        s.fixed2D = true;
    }, function (s) {
        var ayZero = s.ay instanceof CSSUnitValue && s.ay.value === 0;
        return 'skew(' + s.ax + (ayZero ? '' : ', ' + s.ay) + ')';
    });
    axisAccessors(CSSSkew, ['ax', 'ay'], checkAngle);

    var CSSSkewX = transformSubclass('CSSSkewX', function (s, ax) {
        s.ax = checkAngle(ax);
        s.is2D = true;
        s.fixed2D = true;
    }, function (s) { return 'skewX(' + s.ax + ')'; });
    axisAccessors(CSSSkewX, ['ax'], checkAngle);

    var CSSSkewY = transformSubclass('CSSSkewY', function (s, ay) {
        s.ay = checkAngle(ay);
        s.is2D = true;
        s.fixed2D = true;
    }, function (s) { return 'skewY(' + s.ay + ')'; });
    axisAccessors(CSSSkewY, ['ay'], checkAngle);

    function checkPerspective(v) {
        if (typeof v === 'string') v = new CSSKeywordValue(v);
        if (v instanceof CSSKeywordValue) {
            if (v.value !== 'none') throw new TypeError('Expected none or a <length>');
            return v;
        }
        return checkLength(v);
    }

    var CSSPerspective = transformSubclass('CSSPerspective', function (s, length) {
        s.length = checkPerspective(length);
        s.is2D = false;
        s.fixed2D = true;
    }, function (s) {
        var l = s.length;
        if (l instanceof CSSUnitValue && l.value < 0) l = construct(CSSMathSum, [l]);
        return 'perspective(' + l + ')';
    });
    axisAccessors(CSSPerspective, ['length'], checkPerspective);

    var CSSMatrixComponent = transformSubclass('CSSMatrixComponent', function (s, matrix, options) {
        if (!(matrix instanceof DOMMatrixReadOnly)) throw new TypeError('Expected a DOMMatrixReadOnly');
        s.matrix = matrix;
        s.is2D = options && options.is2D !== undefined ? !!options.is2D : matrix.is2D;
    }, function (s) {
        var m = s.matrix;
        if (s.is2D)
            return 'matrix(' + [m.a, m.b, m.c, m.d, m.e, m.f].map(formatNumber).join(', ') + ')';
        return 'matrix3d(' + Array.prototype.map.call(m.toFloat64Array(), formatNumber).join(', ') + ')';
    });
    accessor(CSSMatrixComponent.prototype, 'matrix',
        function () { return slot(this).matrix; },
        function (m) {
            if (!(m instanceof DOMMatrixReadOnly)) throw new TypeError('Expected a DOMMatrixReadOnly');
            slot(this).matrix = m;
        });

    function absolute(v, unit) {
        var c;
        try { c = v.to(unit); } catch (e) { throw new TypeError('The value cannot be converted to ' + unit); }
        return c.value;
    }

    function componentMatrix(c) {
        var s = slot(c);
        var m = new DOMMatrix();
        if (c instanceof CSSTranslate) {
            return s.is2D ? m.translateSelf(absolute(s.x, 'px'), absolute(s.y, 'px'))
                : m.translateSelf(absolute(s.x, 'px'), absolute(s.y, 'px'), absolute(s.z, 'px'));
        }
        if (c instanceof CSSRotate) {
            var deg = absolute(s.angle, 'deg');
            return s.is2D ? m.rotateSelf(deg)
                : m.rotateAxisAngleSelf(s.x.to('number').value, s.y.to('number').value,
                                        s.z.to('number').value, deg);
        }
        if (c instanceof CSSScale) {
            return s.is2D ? m.scaleSelf(s.x.to('number').value, s.y.to('number').value)
                : m.scaleSelf(s.x.to('number').value, s.y.to('number').value, s.z.to('number').value);
        }
        if (c instanceof CSSSkew) {
            var tx = Math.tan(absolute(s.ax, 'rad')), ty = Math.tan(absolute(s.ay, 'rad'));
            return new DOMMatrix([1, ty, tx, 1, 0, 0]);
        }
        if (c instanceof CSSSkewX) return m.skewXSelf(absolute(s.ax, 'deg'));
        if (c instanceof CSSSkewY) return m.skewYSelf(absolute(s.ay, 'deg'));
        if (c instanceof CSSPerspective) {
            if (s.length instanceof CSSKeywordValue) return m;
            var d = absolute(s.length, 'px');
            if (d !== 0) m.m34 = -1 / Math.max(d, 1);
            return m;
        }
        if (c instanceof CSSMatrixComponent) {
            var src = s.matrix;
            return s.is2D ? new DOMMatrix([src.a, src.b, src.c, src.d, src.e, src.f])
                : DOMMatrix.fromFloat64Array(src.toFloat64Array());
        }
        return m;
    }

    function checkComponent(c) {
        if (!(c instanceof CSSTransformComponent)) throw new TypeError('Expected a CSSTransformComponent');
        return c;
    }

    function CSSTransformValue(transforms) {
        if (new.target === undefined) throw new TypeError("Constructor requires 'new'");
        if (transforms === undefined || transforms === null || typeof transforms[Symbol.iterator] !== 'function')
            throw new TypeError('CSSTransformValue requires a sequence');
        var items = Array.from(transforms, checkComponent);
        if (!items.length) throw new TypeError('CSSTransformValue requires at least one component');
        internalConstruct = true;
        try { CSSStyleValue.call(this); } finally { internalConstruct = false; }
        slot(this).items = items;
        var proxy = listProxy(this, items, checkComponent);
        slots.set(proxy, slot(this));
        return proxy;
    }
    CSSTransformValue.prototype = Object.create(CSSStyleValue.prototype, {
        constructor: { value: CSSTransformValue, writable: true, configurable: true }
    });
    Object.setPrototypeOf(CSSTransformValue, CSSStyleValue);
    accessor(CSSTransformValue.prototype, 'length', function () { return slot(this).items.length; });
    accessor(CSSTransformValue.prototype, 'is2D', function () {
        return slot(this).items.every(function (c) { return c.is2D; });
    });
    CSSTransformValue.prototype.toMatrix = function () {
        var m = new DOMMatrix();
        slot(this).items.forEach(function (c) { m.multiplySelf(c.toMatrix()); });
        return m;
    };
    CSSTransformValue.prototype[Symbol.iterator] = function () { return slot(this).items.slice()[Symbol.iterator](); };
    CSSTransformValue.prototype.entries = function () { return slot(this).items.slice().entries(); };
    CSSTransformValue.prototype.keys = function () { return slot(this).items.slice().keys(); };
    CSSTransformValue.prototype.values = function () { return slot(this).items.slice().values(); };
    CSSTransformValue.prototype.forEach = function (cb, thisArg) {
        var items = slot(this).items.slice();
        for (var i = 0; i < items.length; i++) cb.call(thisArg, items[i], i, this);
    };
    CSSTransformValue.prototype.toString = function () {
        return slot(this).items.map(function (c) { return c.toString(); }).join(' ');
    };

    var NUMBER_RE = /^[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?/;
    var IDENT_RE = /^-?(?:[A-Za-z_\u0080-￿]|\\.)(?:[\w\u0080-￿-]|\\.)*/;

    function matchParen(text, open) {
        var depth = 0, quote = null;
        for (var i = open; i < text.length; i++) {
            var ch = text[i];
            if (quote) {
                if (ch === '\\') i++;
                else if (ch === quote) quote = null;
                continue;
            }
            if (ch === '"' || ch === "'") quote = ch;
            else if (ch === '(') depth++;
            else if (ch === ')') { depth--; if (depth === 0) return i; }
        }
        return -1;
    }

    function tokenizeMath(text) {
        var tokens = [], i = 0;
        function operandBefore() {
            var t = tokens[tokens.length - 1];
            return t && !t.op;
        }
        while (i < text.length) {
            var ch = text[i];
            if (/\s/.test(ch)) { i++; continue; }
            if ((ch === '+' || ch === '-') && operandBefore()) { tokens.push({ op: ch }); i++; continue; }
            if (ch === '*' || ch === '/') { tokens.push({ op: ch }); i++; continue; }
            if (ch === '(') {
                var close = matchParen(text, i);
                if (close < 0) throw syntaxError();
                tokens.push({ type: 'group', body: text.slice(i + 1, close) });
                i = close + 1;
                continue;
            }
            var rest = text.slice(i);
            var num = NUMBER_RE.exec(rest);
            if (num) {
                var after = rest.slice(num[0].length);
                var unit = 'number';
                if (after[0] === '%') { unit = 'percent'; i += num[0].length + 1; }
                else {
                    var id = IDENT_RE.exec(after);
                    if (id && !(id[0][0] === '-' && /^-\d/.test(after))) {
                        unit = normalizeUnit(id[0]);
                        if (unit === null || unit === 'number' || unit === 'percent') throw syntaxError('Invalid unit');
                        i += num[0].length + id[0].length;
                    } else {
                        i += num[0].length;
                    }
                }
                tokens.push({ type: 'num', value: parseFloat(num[0]), unit: unit });
                continue;
            }
            var ident = IDENT_RE.exec(rest);
            if (ident && rest[ident[0].length] === '(') {
                var open = i + ident[0].length;
                var end = matchParen(text, open);
                if (end < 0) throw syntaxError();
                tokens.push({ type: 'fn', name: ident[0].toLowerCase(), body: text.slice(open + 1, end) });
                i = end + 1;
                continue;
            }
            throw syntaxError();
        }
        return tokens;
    }

    function splitTopLevel(text, sep) {
        var parts = [], depth = 0, start = 0, quote = null;
        for (var i = 0; i < text.length; i++) {
            var ch = text[i];
            if (quote) {
                if (ch === '\\') i++;
                else if (ch === quote) quote = null;
                continue;
            }
            if (ch === '"' || ch === "'") quote = ch;
            else if (ch === '(') depth++;
            else if (ch === ')') depth--;
            else if (depth === 0 && (sep === ',' ? ch === ',' : /\s/.test(ch))) {
                parts.push(text.slice(start, i));
                start = i + 1;
            }
        }
        parts.push(text.slice(start));
        return sep === ',' ? parts.map(function (p) { return p.trim(); })
            : parts.filter(function (p) { return p !== ''; });
    }

    function canonicalKey(v) {
        var c = CANONICAL[v.unit];
        return c ? c[0] : v.unit;
    }

    function simplifySum(items) {
        if (items.length === 1) return items[0];
        var flat = [];
        items.forEach(function (it) {
            if (it instanceof CSSMathSum) flat = flat.concat(toArray(it.values));
            else flat.push(it);
        });
        var groups = [];
        flat.forEach(function (it) {
            var unitValue = it instanceof CSSUnitValue ? it
                : it instanceof CSSMathNegate && it.value instanceof CSSUnitValue ? it.value : null;
            if (!unitValue) { groups.push({ items: [it] }); return; }
            var key = canonicalKey(unitValue);
            var sign = unitValue === it ? 1 : -1;
            var c = CANONICAL[unitValue.unit];
            var amount = sign * unitValue.value * (c ? c[1] : 1);
            for (var g = 0; g < groups.length; g++) {
                if (groups[g].key === key) {
                    groups[g].items.push(it);
                    groups[g].total += amount;
                    return;
                }
            }
            groups.push({ key: key, items: [it], total: amount });
        });
        var out = groups.map(function (g) {
            if (g.items.length === 1) return g.items[0];
            return makeUnit(g.total, g.key);
        });
        if (out.length === 1) return out[0];
        return construct(CSSMathSum, out);
    }

    function simplifyProduct(items) {
        if (items.length === 1) return items[0];
        var k = 1, others = [];
        items.forEach(function (it) {
            if (it instanceof CSSUnitValue && it.unit === 'number') k *= it.value;
            else others.push(it);
        });
        if (!others.length) return makeUnit(k, 'number');
        if (others.length === 1 && others[0] instanceof CSSUnitValue) return makeUnit(k * others[0].value, others[0].unit);
        var list = (k !== 1 ? [makeUnit(k, 'number')] : []).concat(others);
        if (list.length === 1) return list[0];
        return construct(CSSMathProduct, list);
    }

    function parseExpression(text) {
        var tokens = tokenizeMath(text);
        var i = 0;
        function primary() {
            var t = tokens[i++];
            if (!t || t.op) throw syntaxError();
            if (t.type === 'num') return makeUnit(t.value, t.unit);
            if (t.type === 'group') return parseExpression(t.body);
            return parseMathFunction(t.name, t.body, true);
        }
        function product() {
            var items = [primary()];
            while (tokens[i] && (tokens[i].op === '*' || tokens[i].op === '/')) {
                var op = tokens[i++].op;
                var r = primary();
                items.push(op === '/' ? invertValue(r) : r);
            }
            return simplifyProduct(items);
        }
        function sum() {
            var items = [product()];
            while (tokens[i] && (tokens[i].op === '+' || tokens[i].op === '-')) {
                var op = tokens[i++].op;
                var r = product();
                items.push(op === '-' ? (r instanceof CSSMathNegate ? r.value : new CSSMathNegate(r)) : r);
            }
            return simplifySum(items);
        }
        var result = sum();
        if (i !== tokens.length) throw syntaxError();
        return result;
    }

    function parseMathFunction(name, body, nested) {
        if (name === 'calc' || name === '-webkit-calc') {
            var node = parseExpression(body);
            if (nested) return node;
            if (node instanceof CSSMathValue && !(node instanceof CSSMathNegate) && !(node instanceof CSSMathInvert))
                return node;
            return construct(CSSMathSum, [node]);
        }
        var args = splitTopLevel(body, ',');
        if (args.some(function (a) { return a === ''; })) throw syntaxError();
        var parsed = args.map(parseExpression);
        if (name === 'min') return construct(CSSMathMin, parsed);
        if (name === 'max') return construct(CSSMathMax, parsed);
        if (name === 'clamp' && parsed.length === 3) return new CSSMathClamp(parsed[0], parsed[1], parsed[2]);
        throw syntaxError();
    }

    function parseNumeric(text) {
        var tokens = tokenizeMath(text);
        if (tokens.length !== 1) return null;
        var t = tokens[0];
        if (t.type === 'num') return makeUnit(t.value, t.unit);
        if (t.type === 'fn') {
            try { return parseMathFunction(t.name, t.body, false); }
            catch (e) { return null; }
        }
        return null;
    }

    var CSS_WIDE = { initial: 1, inherit: 1, unset: 1, revert: 1, 'revert-layer': 1 };

    var LIST_VALUED = {};
    ['animation-name', 'animation-duration', 'animation-timing-function', 'animation-iteration-count',
     'animation-direction', 'animation-play-state', 'animation-delay', 'animation-fill-mode',
     'animation-composition', 'animation-timeline', 'transition-property', 'transition-duration',
     'transition-timing-function', 'transition-delay', 'transition-behavior', 'background-image',
     'background-position', 'background-position-x', 'background-position-y', 'background-size',
     'background-repeat', 'background-attachment', 'background-origin', 'background-clip',
     'background-blend-mode', 'mask-image', 'mask-mode', 'mask-position', 'mask-size', 'mask-repeat',
     'mask-origin', 'mask-clip', 'mask-composite', 'scroll-timeline-name', 'scroll-timeline-axis',
     'view-timeline-name', 'view-timeline-axis', 'will-change'].forEach(function (p) { LIST_VALUED[p] = 1; });

    var SHORTHANDS = {};
    ['all', 'animation', 'background', 'background-position', 'border', 'border-block', 'border-block-color',
     'border-block-end', 'border-block-start', 'border-block-style', 'border-block-width', 'border-bottom',
     'border-color', 'border-image', 'border-inline', 'border-inline-color', 'border-inline-end',
     'border-inline-start', 'border-inline-style', 'border-inline-width', 'border-left', 'border-radius',
     'border-right', 'border-style', 'border-top', 'border-width', 'column-rule', 'columns', 'contain-intrinsic-size',
     'container', 'flex', 'flex-flow', 'font', 'font-synthesis', 'font-variant', 'gap', 'grid', 'grid-area',
     'grid-column', 'grid-gap', 'grid-row', 'grid-template', 'inset', 'inset-block', 'inset-inline',
     'list-style', 'margin', 'margin-block', 'margin-inline', 'marker', 'mask', 'mask-border', 'offset',
     'outline', 'overflow', 'overscroll-behavior', 'padding', 'padding-block', 'padding-inline',
     'place-content', 'place-items', 'place-self', 'scroll-margin', 'scroll-margin-block',
     'scroll-margin-inline', 'scroll-padding', 'scroll-padding-block', 'scroll-padding-inline',
     'scroll-timeline', 'text-decoration', 'text-emphasis', 'transition', 'view-timeline',
     'white-space', 'text-wrap', 'animation-range', 'position-try', 'contain-intrinsic-size',
     '-webkit-text-stroke', '-webkit-mask', 'word-wrap'].forEach(function (p) { SHORTHANDS[p] = 1; });

    var IMAGE_PROPERTIES = {
        'background-image': 1, 'border-image-source': 1, 'list-style-image': 1, 'mask-image': 1,
        'shape-outside': 1, 'mask-border-source': 1, '-webkit-mask-image': 1, 'content': 1, 'cursor': 1
    };

    var supportCache = new Map();
    function supports(property, text) {
        var key = property + ':' + text;
        var r = supportCache.get(key);
        if (r === undefined) {
            try { r = !!global.CSS.supports(property, text); } catch (e) { r = false; }
            if (supportCache.size > 4096) supportCache.clear();
            supportCache.set(key, r);
        }
        return r;
    }

    function isCustom(property) { return property.slice(0, 2) === '--'; }

    function normalizeProperty(property) {
        var p = String(property);
        if (isCustom(p)) return p;
        p = p.toLowerCase();
        if (!supports(p, 'inherit')) throw new TypeError("'" + property + "' is not a valid property");
        return p;
    }

    function hasVar(text) { return /\bvar\(/i.test(text); }

    function parseUnparsed(text) {
        var members = [], buf = '', i = 0;
        while (i < text.length) {
            var rest = text.slice(i);
            var m = /^var\(/i.exec(rest);
            if (m) {
                var close = matchParen(text, i + 3);
                if (close < 0) { buf += text.slice(i); break; }
                if (buf) { members.push(buf); buf = ''; }
                var body = text.slice(i + 4, close);
                var comma = splitTopLevel(body, ',');
                var name = comma[0].trim();
                var fallback = null;
                if (comma.length > 1) {
                    var idx = body.indexOf(',');
                    fallback = new CSSUnparsedValue(parseUnparsed(body.slice(idx + 1)));
                }
                members.push(new CSSVariableReferenceValue(name, fallback));
                i = close + 1;
                continue;
            }
            var ch = text[i];
            if (ch === '"' || ch === "'") {
                var j = i + 1;
                while (j < text.length && text[j] !== ch) { if (text[j] === '\\') j++; j++; }
                buf += text.slice(i, j + 1);
                i = j + 1;
                continue;
            }
            buf += ch;
            i++;
        }
        if (buf) members.push(buf);
        return members;
    }

    function acceptsNumber(property) {
        return supports(property, '1') || supports(property, '1.5');
    }

    var FONT_RELATIVE = { em: 1, ex: 0.5, ch: 0.5, ic: 1, cap: 0.7, lh: 1.2 };
    var ROOT_RELATIVE = { rem: 1, rex: 0.5, rch: 0.5, ric: 1, rcap: 0.7, rlh: 1.2 };

    function absolutizeUnit(v, context) {
        var unit = v.unit, value = v.value;
        if (CANONICAL[unit]) return makeUnit(value * CANONICAL[unit][1], CANONICAL[unit][0]);
        if (!context) return v;
        if (FONT_RELATIVE[unit]) return makeUnit(value * FONT_RELATIVE[unit] * context.font(), 'px');
        if (ROOT_RELATIVE[unit]) return makeUnit(value * ROOT_RELATIVE[unit] * context.root(), 'px');
        var vw = (global.innerWidth || 0) / 100, vh = (global.innerHeight || 0) / 100;
        var base = unit.replace(/^[sld]/, '');
        var per = { vw: vw, vi: vw, vh: vh, vb: vh, vmin: Math.min(vw, vh), vmax: Math.max(vw, vh) }[base];
        if (per !== undefined) return makeUnit(value * per, 'px');
        return v;
    }

    function computeNumeric(v, context) {
        if (v instanceof CSSUnitValue) return absolutizeUnit(v, context);
        var sum = sumValue(mapUnits(v, function (u) { return absolutizeUnit(u, context); }));
        if (sum && sum.length === 1) {
            var single = sumItemToUnit(sum[0]);
            if (single) return single;
        }
        return v;
    }

    function mapUnits(v, fn) {
        if (v instanceof CSSUnitValue) return fn(v);
        var s = slot(v);
        if (s.operator === 'negate') return new CSSMathNegate(mapUnits(s.value, fn));
        if (s.operator === 'invert') return new CSSMathInvert(mapUnits(s.value, fn));
        if (s.operator === 'clamp') return new CSSMathClamp(mapUnits(s.lower, fn), mapUnits(s.value, fn), mapUnits(s.upper, fn));
        var ctor = { sum: CSSMathSum, product: CSSMathProduct, min: CSSMathMin, max: CSSMathMax }[s.operator];
        return construct(ctor, s.items.map(function (i) { return mapUnits(i, fn); }));
    }

    var TRANSFORM_ARITY = {
        translate: [1, 2], translatex: [1, 1], translatey: [1, 1], translatez: [1, 1], translate3d: [3, 3],
        rotate: [1, 1], rotatex: [1, 1], rotatey: [1, 1], rotatez: [1, 1], rotate3d: [4, 4],
        scale: [1, 2], scalex: [1, 1], scaley: [1, 1], scalez: [1, 1], scale3d: [3, 3],
        skew: [1, 2], skewx: [1, 1], skewy: [1, 1], perspective: [1, 1], matrix: [6, 6], matrix3d: [16, 16]
    };

    function transformArg(text, kind, context) {
        var v = parseNumeric(text.trim());
        if (!v) {
            if (kind === 'perspective' && text.trim().toLowerCase() === 'none') return new CSSKeywordValue('none');
            throw syntaxError();
        }
        if (v instanceof CSSUnitValue && v.unit === 'number' && v.value === 0 && (kind === 'length' || kind === 'perspective'))
            v = makeUnit(0, 'px');
        if (v instanceof CSSUnitValue && v.unit === 'number' && v.value === 0 && kind === 'angle')
            v = makeUnit(0, 'deg');
        if (context && v instanceof CSSNumericValue) v = computeNumeric(v, context);
        return v;
    }

    function parseTransformList(text, context) {
        var fns = splitTopLevel(text, ' ');
        if (!fns.length) return null;
        var components = fns.map(function (fn) {
            var m = /^([A-Za-z0-9-]+)\(([\s\S]*)\)$/.exec(fn);
            if (!m) throw syntaxError();
            var name = m[1].toLowerCase();
            var arity = TRANSFORM_ARITY[name];
            var args = splitTopLevel(m[2], ',');
            if (!arity || args.length < arity[0] || args.length > arity[1]) throw syntaxError();
            var L = function (i) { return transformArg(args[i], 'length', context); };
            var A = function (i) { return transformArg(args[i], 'angle', context); };
            var N = function (i) { return transformArg(args[i], 'number', context); };
            var px0 = makeUnit(0, 'px');
            switch (name) {
            case 'translate': return new CSSTranslate(L(0), args.length > 1 ? L(1) : px0);
            case 'translatex': return new CSSTranslate(L(0), px0);
            case 'translatey': return new CSSTranslate(px0, L(0));
            case 'translatez': return new CSSTranslate(px0, px0, L(0));
            case 'translate3d': return new CSSTranslate(L(0), L(1), L(2));
            case 'rotate': return new CSSRotate(A(0));
            case 'rotatex': return new CSSRotate(1, 0, 0, A(0));
            case 'rotatey': return new CSSRotate(0, 1, 0, A(0));
            case 'rotatez': return new CSSRotate(0, 0, 1, A(0));
            case 'rotate3d': return new CSSRotate(N(0), N(1), N(2), A(3));
            case 'scale': return new CSSScale(N(0), args.length > 1 ? N(1) : N(0));
            case 'scalex': return new CSSScale(N(0), 1);
            case 'scaley': return new CSSScale(1, N(0));
            case 'scalez': return new CSSScale(1, 1, N(0));
            case 'scale3d': return new CSSScale(N(0), N(1), N(2));
            case 'skew': return new CSSSkew(A(0), args.length > 1 ? A(1) : makeUnit(0, 'deg'));
            case 'skewx': return new CSSSkewX(A(0));
            case 'skewy': return new CSSSkewY(A(0));
            case 'perspective': return new CSSPerspective(transformArg(args[0], 'perspective', context));
            default:
                var nums = args.map(function (a) {
                    var n = parseNumeric(a.trim());
                    if (!(n instanceof CSSUnitValue) || n.unit !== 'number') throw syntaxError();
                    return n.value;
                });
                return new CSSMatrixComponent(new DOMMatrixReadOnly(nums));
            }
        });
        return new CSSTransformValue(components);
    }

    var DASHED_IDENT_RE = /^--[\w\u0080-￿-]+$/;
    var OPAQUE_PROPERTIES = { 'font-family': 1, 'transition-property': 1, 'will-change': 1 };

    function isColorProperty(property) {
        return /(^|-)color$/.test(property) || property === 'fill' || property === 'stroke';
    }

    function reifyComponent(property, text, context) {
        var t = text.trim();
        var lower = t.toLowerCase();
        if (CSS_WIDE[lower]) return new CSSKeywordValue(lower);
        if (isCustom(property)) return new CSSUnparsedValue(t === '' ? [] : parseUnparsed(t));
        if (hasVar(t)) return new CSSUnparsedValue(parseUnparsed(t));
        if (SHORTHANDS[property]) return makeOpaque(t, property);
        if (property === 'transform' && lower !== 'none') {
            try { return parseTransformList(t, context); } catch (e) { return makeOpaque(t, property); }
        }
        if (OPAQUE_PROPERTIES[property]) return makeOpaque(t, property);
        var tokens = splitTopLevel(t, ' ');
        if (tokens.length === 1) {
            if (DASHED_IDENT_RE.test(t)) return new CSSKeywordValue(t);
            if (IDENT_RE.test(t) && IDENT_RE.exec(t)[0] === t) {
                if (lower !== 'currentcolor' && isColorProperty(property) && supports('color', t))
                    return makeOpaque(t, property);
                return new CSSKeywordValue(t);
            }
            var numeric = null;
            try { numeric = parseNumeric(t); } catch (e) { numeric = null; }
            if (numeric) {
                if (numeric instanceof CSSUnitValue && numeric.unit === 'number' && numeric.value === 0 &&
                    !acceptsNumber(property))
                    numeric = makeUnit(0, 'px');
                return context ? computeNumeric(numeric, context) : numeric;
            }
            if (IMAGE_PROPERTIES[property] && /^url\(/i.test(t)) return makeOpaque(t, property, CSSImageValue);
        }
        return makeOpaque(t, property);
    }

    function splitList(property, text) {
        if (!LIST_VALUED[property] || hasVar(text) || CSS_WIDE[text.trim().toLowerCase()]) return [text];
        return splitTopLevel(text, ',');
    }

    function reifyList(property, text, context) {
        return splitList(property, text).map(function (part) {
            internalConstruct = true;
            try { return reifyComponent(property, part, context); } finally { internalConstruct = false; }
        });
    }

    function parseForProperty(property, cssText, all) {
        var p = normalizeProperty(property);
        var text = String(cssText).trim();
        if (!isCustom(p) && !supports(p, text)) throw new TypeError("'" + cssText + "' is not valid for '" + p + "'");
        var values = reifyList(p, text, null);
        return all ? values : values[0];
    }

    CSSStyleValue.parse = function (property, cssText) { return parseForProperty(property, cssText, false); };
    CSSStyleValue.parseAll = function (property, cssText) { return parseForProperty(property, cssText, true); };

    function valueText(property, value, single) {
        if (typeof value === 'string' || value instanceof String) {
            var text = String(value).trim();
            if (!isCustom(property) && !supports(property, text)) throw new TypeError('Invalid value for ' + property);
            if (!single && (hasVar(text) || CSS_WIDE[text.toLowerCase()]))
                throw new TypeError('A var() reference or CSS-wide keyword must be the only value');
            if (!LIST_VALUED[property] && !isCustom(property) && splitTopLevel(text, ',').length > 1 &&
                !supports(property, text))
                throw new TypeError('Not a list-valued property');
            return text;
        }
        if (!(value instanceof CSSStyleValue)) throw new TypeError('Expected a CSSStyleValue or string');
        var s = slots.get(value) || {};
        if (s.property !== undefined && s.property !== property) throw new TypeError('The value belongs to another property');
        if (value instanceof CSSUnparsedValue) {
            if (!single) throw new TypeError('A CSSUnparsedValue must be the only value');
            var u = value.toString();
            if (isCustom(property)) return u;
            if (!supports(property, u)) throw new TypeError('Invalid value for ' + property);
            return u;
        }
        if (isCustom(property)) throw new TypeError('Custom properties take a CSSUnparsedValue');
        var css = value.toString();
        if (value instanceof CSSKeywordValue && CSS_WIDE[value.value.toLowerCase()] && !single)
            throw new TypeError('A CSS-wide keyword must be the only value');
        if (value instanceof CSSUnitValue && value.unit === 'number') {
            var calcText = 'calc(' + css + ')';
            var calcOk = supports(property, calcText);
            if (supports(property, css) && (value.value !== 0 || calcOk || !supports(property, '1px'))) return css;
            if (calcOk) return calcText;
            throw new TypeError('Invalid value for ' + property);
        }
        if (supports(property, css)) return css;
        if (value instanceof CSSNumericValue) {
            var wrapped = value instanceof CSSUnitValue ? 'calc(' + css + ')' : css;
            if (wrapped !== css && supports(property, wrapped)) return wrapped;
        }
        throw new TypeError('Invalid value for ' + property);
    }

    function StylePropertyMapReadOnly() {
        if (!internalConstruct) throw new TypeError('Illegal constructor');
    }

    function makeMap(ctor, backing) {
        internalConstruct = true;
        var m;
        try { m = new ctor(); } finally { internalConstruct = false; }
        slots.set(m, backing);
        return m;
    }

    function backingText(map, property) {
        var b = slot(map);
        return b.read(property);
    }

    StylePropertyMapReadOnly.prototype.get = function (property) {
        var p = normalizeProperty(property);
        var text = backingText(this, p);
        if (text === '' || text === null || text === undefined) return undefined;
        return reifyList(p, text, slot(this).context)[0];
    };
    StylePropertyMapReadOnly.prototype.getAll = function (property) {
        var p = normalizeProperty(property);
        var text = backingText(this, p);
        if (text === '' || text === null || text === undefined) return [];
        return reifyList(p, text, slot(this).context);
    };
    StylePropertyMapReadOnly.prototype.has = function (property) {
        var p = normalizeProperty(property);
        var text = backingText(this, p);
        return !(text === '' || text === null || text === undefined);
    };
    accessor(StylePropertyMapReadOnly.prototype, 'size', function () {
        return slot(this).names().length;
    });
    function mapEntries(map) {
        var b = slot(map);
        var out = [];
        b.names().forEach(function (name) {
            var text = b.read(name);
            if (text === '' || text === null || text === undefined) return;
            out.push([name, reifyList(name, text, b.context)]);
        });
        return out;
    }
    StylePropertyMapReadOnly.prototype.entries = function () { return mapEntries(this)[Symbol.iterator](); };
    StylePropertyMapReadOnly.prototype[Symbol.iterator] = StylePropertyMapReadOnly.prototype.entries;
    StylePropertyMapReadOnly.prototype.keys = function () {
        return mapEntries(this).map(function (e) { return e[0]; })[Symbol.iterator]();
    };
    StylePropertyMapReadOnly.prototype.values = function () {
        return mapEntries(this).map(function (e) { return e[1]; })[Symbol.iterator]();
    };
    StylePropertyMapReadOnly.prototype.forEach = function (cb, thisArg) {
        var self = this;
        mapEntries(this).forEach(function (e) { cb.call(thisArg, e[1], e[0], self); });
    };

    function StylePropertyMap() {
        if (!internalConstruct) throw new TypeError('Illegal constructor');
    }
    StylePropertyMap.prototype = Object.create(StylePropertyMapReadOnly.prototype, {
        constructor: { value: StylePropertyMap, writable: true, configurable: true }
    });
    Object.setPrototypeOf(StylePropertyMap, StylePropertyMapReadOnly);

    StylePropertyMap.prototype.set = function (property) {
        var p = normalizeProperty(property);
        var values = Array.prototype.slice.call(arguments, 1);
        if (values.length > 1 && !LIST_VALUED[p]) throw new TypeError("'" + p + "' is not list-valued");
        var single = values.length === 1;
        var texts = values.map(function (v) { return valueText(p, v, single); });
        if (texts.length === 1 && !LIST_VALUED[p] && !isCustom(p) && typeof values[0] === 'string' &&
            !supports(p, texts[0]))
            throw new TypeError('Invalid value');
        slot(this).write(p, texts.join(', '));
    };

    StylePropertyMap.prototype.append = function (property) {
        var p = normalizeProperty(property);
        if (!LIST_VALUED[p]) throw new TypeError("'" + p + "' is not list-valued");
        var values = Array.prototype.slice.call(arguments, 1);
        var current = slot(this).read(p) || '';
        if (current && (hasVar(current) || CSS_WIDE[current.trim().toLowerCase()]))
            throw new TypeError('Cannot append to a var() reference or CSS-wide keyword');
        if (!values.length) return;
        var texts = values.map(function (v) {
            if (typeof v === 'string' && (hasVar(v) || CSS_WIDE[v.trim().toLowerCase()]))
                throw new TypeError('Cannot append a var() reference or CSS-wide keyword');
            if (v instanceof CSSUnparsedValue) throw new TypeError('Cannot append a CSSUnparsedValue');
            if (v instanceof CSSKeywordValue && CSS_WIDE[v.value.toLowerCase()])
                throw new TypeError('Cannot append a CSS-wide keyword');
            return valueText(p, v, false);
        });
        slot(this).write(p, (current ? current + ', ' : '') + texts.join(', '));
    };

    StylePropertyMap.prototype.delete = function (property) {
        var p = normalizeProperty(property);
        slot(this).remove(p);
    };

    StylePropertyMap.prototype.clear = function () {
        slot(this).clear();
    };

    function declarationBacking(getDecl) {
        return {
            context: null,
            read: function (p) { return getDecl().getPropertyValue(p); },
            write: function (p, text) {
                var d = getDecl();
                d.setProperty(p, text);
                if (d.getPropertyValue(p) === '' && text !== '') throw new TypeError('Invalid value');
            },
            remove: function (p) { getDecl().removeProperty(p); },
            clear: function () { getDecl().cssText = ''; },
            names: function () {
                var d = getDecl(), out = [], seen = {};
                for (var i = 0; i < d.length; i++) {
                    var n = d.item ? d.item(i) : d[i];
                    if (n && !seen[n]) { seen[n] = 1; out.push(n); }
                }
                return sortNames(out);
            }
        };
    }

    function sortNames(names) {
        var standard = names.filter(function (n) { return !isCustom(n); });
        var custom = names.filter(isCustom);
        standard.sort(function (a, b) {
            if (a.startsWith('-') === b.startsWith('-')) return a < b ? -1 : a > b ? 1 : 0;
            return b.startsWith('-') ? -1 : 1;
        });
        custom.sort(function (a, b) {
            var ca = Array.from(a), cb = Array.from(b);
            for (var i = 0; i < Math.min(ca.length, cb.length); i++) {
                var x = ca[i].codePointAt(0), y = cb[i].codePointAt(0);
                if (x !== y) return x - y;
            }
            return ca.length - cb.length;
        });
        return standard.concat(custom);
    }

    function computedBacking(element) {
        var style = function () { return global.getComputedStyle(element); };
        var px = function (text) { var n = parseFloat(text); return isFinite(n) ? n : 16; };
        return {
            context: {
                font: function () { return px(style().getPropertyValue('font-size')); },
                root: function () {
                    var r = element.ownerDocument && element.ownerDocument.documentElement;
                    return r ? px(global.getComputedStyle(r).getPropertyValue('font-size')) : 16;
                }
            },
            read: function (p) {
                if (nativeComputedValue && !isCustom(p)) return nativeComputedValue(element, p);
                return style().getPropertyValue(p);
            },
            names: function () {
                var s = style(), out = [];
                for (var i = 0; i < s.length; i++) out.push(s.item ? s.item(i) : s[i]);
                return sortNames(out);
            }
        };
    }

    var inlineMaps = new WeakMap();
    var ruleMaps = new WeakMap();

    function attributeStyleMapGetter() {
        if (!(this instanceof Element)) throw new TypeError('Illegal invocation');
        var m = inlineMaps.get(this);
        if (!m) {
            var el = this;
            m = makeMap(StylePropertyMap, declarationBacking(function () { return el.style; }));
            inlineMaps.set(this, m);
        }
        return m;
    }

    ['HTMLElement', 'SVGElement', 'MathMLElement'].forEach(function (name) {
        var ctor = global[name];
        if (typeof ctor !== 'function' || !ctor.prototype) return;
        Object.defineProperty(ctor.prototype, 'attributeStyleMap', {
            get: attributeStyleMapGetter, enumerable: true, configurable: true
        });
    });

    if (typeof global.Element === 'function') {
        Object.defineProperty(Element.prototype, 'computedStyleMap', {
            value: function computedStyleMap() {
                if (!(this instanceof Element)) throw new TypeError('Illegal invocation');
                return makeMap(StylePropertyMapReadOnly, computedBacking(this));
            },
            writable: true, enumerable: true, configurable: true
        });
    }

    if (typeof global.CSSStyleRule === 'function') {
        Object.defineProperty(CSSStyleRule.prototype, 'styleMap', {
            get: function () {
                var m = ruleMaps.get(this);
                if (!m) {
                    var rule = this;
                    m = makeMap(StylePropertyMap, declarationBacking(function () { return rule.style; }));
                    ruleMaps.set(this, m);
                }
                return m;
            },
            enumerable: true, configurable: true
        });
    }

    function colorChannel(kind, input, isDefault) {
        if (input === undefined && isDefault) return makeUnit(100, 'percent');
        if (kind === 'hue') {
            if (typeof input === 'number') return makeUnit(finiteDouble(input), 'deg');
            if (input instanceof CSSKeywordValue) return input;
            if (input instanceof CSSNumericValue) {
                if (!isAngleType(numericType(input))) throw syntaxError('Expected an angle');
                return input;
            }
            return new CSSKeywordValue(String(input));
        }
        if (input instanceof CSSKeywordValue) {
            if (input.value.toLowerCase() !== 'none') throw syntaxError('Expected none');
            return input;
        }
        if (typeof input === 'number') {
            var n = finiteDouble(input);
            return kind === 'number' ? makeUnit(n, 'number') : makeUnit(n * 100, 'percent');
        }
        if (!(input instanceof CSSNumericValue)) throw syntaxError('Expected a number or percentage');
        var t = numericType(input);
        if (onlyBase(t, 'percent') && !t.hint) return input;
        if (isNumberType(t) && kind !== 'percent') return input;
        throw syntaxError('Invalid color channel');
    }

    function CSSColorValue() {
        if (!internalConstruct && (new.target === CSSColorValue || new.target === undefined))
            throw new TypeError('Illegal constructor');
        internalConstruct = true;
        try { CSSStyleValue.call(this); } finally { internalConstruct = false; }
    }
    CSSColorValue.prototype = Object.create(CSSStyleValue.prototype, {
        constructor: { value: CSSColorValue, writable: true, configurable: true }
    });
    Object.setPrototypeOf(CSSColorValue, CSSStyleValue);

    function colorClass(name, fn, channels) {
        var ctor = function () {
            if (new.target === undefined) throw new TypeError("Constructor requires 'new'");
            var args = arguments;
            var values = {};
            channels.forEach(function (c, i) {
                values[c[0]] = colorChannel(c[1], args[i], c[0] === 'alpha');
            });
            internalConstruct = true;
            try { CSSColorValue.call(this); } finally { internalConstruct = false; }
            Object.assign(slot(this), values);
        };
        Object.defineProperty(ctor, 'name', { value: name });
        ctor.prototype = Object.create(CSSColorValue.prototype, {
            constructor: { value: ctor, writable: true, configurable: true }
        });
        Object.setPrototypeOf(ctor, CSSColorValue);
        channels.forEach(function (c) {
            accessor(ctor.prototype, c[0],
                function () { return slot(this)[c[0]]; },
                function (v) { slot(this)[c[0]] = colorChannel(c[1], v, c[0] === 'alpha'); });
        });
        ctor.prototype.toString = function () {
            var s = slot(this);
            var main = channels.slice(0, 3).map(function (c) { return String(s[c[0]]); }).join(' ');
            var opaque = s.alpha instanceof CSSUnitValue && s.alpha.unit === 'percent' && s.alpha.value === 100;
            return fn + '(' + main + (opaque ? '' : ' / ' + s.alpha) + ')';
        };
        define(name, ctor);
        return ctor;
    }

    var CSSRGB = colorClass('CSSRGB', 'rgb', [['r', 'rgb'], ['g', 'rgb'], ['b', 'rgb'], ['alpha', 'percent']]);
    var CSSHSL = colorClass('CSSHSL', 'hsl', [['h', 'hue'], ['s', 'percent'], ['l', 'percent'], ['alpha', 'percent']]);
    var CSSHWB = colorClass('CSSHWB', 'hwb', [['h', 'hue'], ['w', 'percent'], ['b', 'percent'], ['alpha', 'percent']]);
    var CSSLab = colorClass('CSSLab', 'lab', [['l', 'percent'], ['a', 'number'], ['b', 'number'], ['alpha', 'percent']]);
    var CSSLCH = colorClass('CSSLCH', 'lch', [['l', 'percent'], ['c', 'number'], ['h', 'hue'], ['alpha', 'percent']]);
    var CSSOKLab = colorClass('CSSOKLab', 'oklab', [['l', 'percent'], ['a', 'number'], ['b', 'number'], ['alpha', 'percent']]);
    var CSSOKLCH = colorClass('CSSOKLCH', 'oklch', [['l', 'percent'], ['c', 'number'], ['h', 'hue'], ['alpha', 'percent']]);

    var SYSTEM_COLORS = ['accentcolor', 'accentcolortext', 'activetext', 'buttonborder', 'buttonface',
        'buttontext', 'canvas', 'canvastext', 'field', 'fieldtext', 'graytext', 'highlight',
        'highlighttext', 'linktext', 'mark', 'marktext', 'selecteditem', 'selecteditemtext', 'visitedtext',
        'currentcolor'];

    function colorArgs(body) {
        var slash = body.split('/');
        if (slash.length > 2) throw syntaxError();
        var main = slash[0].indexOf(',') >= 0 ? splitTopLevel(slash[0], ',') : splitTopLevel(slash[0].trim(), ' ');
        if (slash.length === 2) main.push(slash[1].trim());
        return main;
    }

    function colorToken(text, kind) {
        var t = text.trim();
        if (t.toLowerCase() === 'none') return new CSSKeywordValue('none');
        var v = parseNumeric(t);
        if (!v) throw syntaxError();
        if (kind === 'alpha' && v instanceof CSSUnitValue && v.unit === 'number') return makeUnit(v.value * 100, 'percent');
        if (kind === 'hue' && v instanceof CSSUnitValue && v.unit === 'number') return makeUnit(v.value, 'deg');
        return v;
    }

    function rgbFromComputed(text) {
        var m = /^rgba?\(([^)]*)\)$/.exec(text);
        if (!m) return null;
        var parts = colorArgs(m[1]);
        var nums = parts.map(function (p) { return parseFloat(p); });
        return new CSSRGB(makeUnit(nums[0], 'number'), makeUnit(nums[1], 'number'), makeUnit(nums[2], 'number'),
                          makeUnit((parts.length > 3 ? nums[3] : 1) * 100, 'percent'));
    }

    var COLOR_FUNCTIONS = {
        rgb: ['rgb', 'n', 'n', 'n'], rgba: ['rgb', 'n', 'n', 'n'],
        hsl: ['hsl', 'hue', 'n', 'n'], hsla: ['hsl', 'hue', 'n', 'n'],
        hwb: ['hwb', 'hue', 'n', 'n'], lab: ['lab', 'n', 'n', 'n'], lch: ['lch', 'n', 'n', 'hue'],
        oklab: ['oklab', 'n', 'n', 'n'], oklch: ['oklch', 'n', 'n', 'hue']
    };
    var COLOR_CTORS = { rgb: CSSRGB, hsl: CSSHSL, hwb: CSSHWB, lab: CSSLab, lch: CSSLCH, oklab: CSSOKLab, oklch: CSSOKLCH };

    CSSColorValue.parse = function (cssText) {
        var text = String(cssText).trim();
        var lower = text.toLowerCase();
        if (SYSTEM_COLORS.indexOf(lower) >= 0) return new CSSKeywordValue(lower);
        var hex = /^#([0-9a-f]{3,4}|[0-9a-f]{6}|[0-9a-f]{8})$/i.exec(text);
        if (hex) {
            var h = hex[1];
            if (h.length <= 4) h = h.split('').map(function (c) { return c + c; }).join('');
            var byteAt = function (i) { return parseInt(h.substr(i * 2, 2), 16); };
            return new CSSRGB(makeUnit(byteAt(0), 'number'), makeUnit(byteAt(1), 'number'), makeUnit(byteAt(2), 'number'),
                              makeUnit(h.length === 8 ? byteAt(3) / 255 * 100 : 100, 'percent'));
        }
        var fn = /^([a-z]+)\(([\s\S]*)\)$/i.exec(text);
        if (fn) {
            var kinds = COLOR_FUNCTIONS[fn[1].toLowerCase()];
            if (!kinds) throw syntaxError();
            var args = colorArgs(fn[2]);
            if (args.length < 3 || args.length > 4) throw syntaxError();
            var alpha = args.length > 3 ? colorToken(args[3], 'alpha') : undefined;
            var vals = [colorToken(args[0], kinds[1]), colorToken(args[1], kinds[2]), colorToken(args[2], kinds[3])];
            try { return new COLOR_CTORS[kinds[0]](vals[0], vals[1], vals[2], alpha); }
            catch (e) { throw syntaxError(); }
        }
        if (/^[a-z]+$/i.test(text) && !CSS_WIDE[lower] && typeof document !== 'undefined' &&
            supports('color', text)) {
            var probe = document.createElement('span');
            probe.style.color = text;
            (document.body || document.documentElement).appendChild(probe);
            var computed = global.getComputedStyle(probe).getPropertyValue('color');
            probe.remove();
            var rgb = rgbFromComputed(computed);
            if (rgb) return rgb;
        }
        throw syntaxError('Not a color');
    };

    define('CSSColorValue', CSSColorValue);
    define('CSSStyleValue', CSSStyleValue);
    define('CSSNumericValue', CSSNumericValue);
    define('CSSUnitValue', CSSUnitValue);
    define('CSSNumericArray', CSSNumericArray);
    define('CSSMathValue', CSSMathValue);
    define('CSSMathSum', CSSMathSum);
    define('CSSMathProduct', CSSMathProduct);
    define('CSSMathMin', CSSMathMin);
    define('CSSMathMax', CSSMathMax);
    define('CSSMathNegate', CSSMathNegate);
    define('CSSMathInvert', CSSMathInvert);
    define('CSSMathClamp', CSSMathClamp);
    define('CSSKeywordValue', CSSKeywordValue);
    define('CSSUnparsedValue', CSSUnparsedValue);
    define('CSSVariableReferenceValue', CSSVariableReferenceValue);
    define('CSSImageValue', CSSImageValue);
    define('CSSTransformComponent', CSSTransformComponent);
    define('CSSTransformValue', CSSTransformValue);
    define('StylePropertyMapReadOnly', StylePropertyMapReadOnly);
    define('StylePropertyMap', StylePropertyMap);

    if (global.CSS) {
        ['number', 'percent', 'em', 'ex', 'ch', 'ic', 'cap', 'rem', 'rex', 'rch', 'ric', 'rcap', 'lh', 'rlh',
         'vw', 'vh', 'vi', 'vb', 'vmin', 'vmax', 'svw', 'svh', 'svi', 'svb', 'svmin', 'svmax',
         'lvw', 'lvh', 'lvi', 'lvb', 'lvmin', 'lvmax', 'dvw', 'dvh', 'dvi', 'dvb', 'dvmin', 'dvmax',
         'cqw', 'cqh', 'cqi', 'cqb', 'cqmin', 'cqmax', 'cm', 'mm', 'Q', 'in', 'pt', 'pc', 'px',
         'deg', 'grad', 'rad', 'turn', 's', 'ms', 'Hz', 'kHz', 'dpi', 'dpcm', 'dppx', 'x', 'fr'
        ].forEach(function (unit) {
            var fn = { [unit]: function (value) { return makeUnit(value, unit); } }[unit];
            try {
                Object.defineProperty(global.CSS, unit, { value: fn, writable: true, configurable: true, enumerable: true });
            } catch (e) {}
        });
    }
})(typeof globalThis !== 'undefined' ? globalThis : this);
